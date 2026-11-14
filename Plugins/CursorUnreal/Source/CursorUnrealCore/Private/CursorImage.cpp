#include "CursorImage.h"

#include "CursorUnrealLog.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shellapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	/** A PNG larger than this is sent as a JPEG instead, when it has no transparency to lose. */
	constexpr int64 LargePngBytes = 1536 * 1024;
	constexpr int32 JpegQuality = 88;
	/** Decoding is refused above this many pixels, so a mislabelled or hostile file cannot take the editor's memory. */
	constexpr int64 MaxSourcePixels = 16384ll * 16384ll;

	IImageWrapperModule& Wrappers()
	{
		return FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	}

	void ForceOpaque(FImage& Image)
	{
		for (FColor& Pixel : Image.AsBGRA8())
		{
			Pixel.A = 255;
		}
	}

	bool Compress(const FImage& Image, EImageFormat Format, TArray<uint8>& Out)
	{
		TArray64<uint8> Encoded;
		if (!Wrappers().CompressImage(Encoded, Format, Image, Format == EImageFormat::JPEG ? JpegQuality : 0) || Encoded.Num() == 0 || Encoded.Num() > MAX_int32)
		{
			return false;
		}
		Out.SetNumUninitialized(static_cast<int32>(Encoded.Num()));
		FMemory::Memcpy(Out.GetData(), Encoded.GetData(), Encoded.Num());
		return true;
	}

	/** Shrinks Image to the size limit and encodes it. Image must be BGRA8. */
	bool Encode(FImage& Image, bool bPhoto, bool bKeepAlpha, const FString& Name, FCursorImage& Out, FString& OutError)
	{
		const int32 LongEdge = FMath::Max(Image.SizeX, Image.SizeY);
		if (LongEdge > CursorImages::MaxLongEdge)
		{
			const double Scale = static_cast<double>(CursorImages::MaxLongEdge) / LongEdge;
			const int32 NewX = FMath::Max(1, FMath::RoundToInt32(Image.SizeX * Scale));
			const int32 NewY = FMath::Max(1, FMath::RoundToInt32(Image.SizeY * Scale));
			FImageCore::ResizeImageInPlace(Image, NewX, NewY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		}

		const bool bTransparent = bKeepAlpha && FImageCore::DetectAlphaChannel(Image);
		if (!bTransparent)
		{
			ForceOpaque(Image);
		}

		TArray<uint8> Bytes;
		bool bJpeg = bPhoto && !bTransparent;
		if (!bJpeg)
		{
			if (!Compress(Image, EImageFormat::PNG, Bytes))
			{
				OutError = FString::Printf(TEXT("'%s' could not be encoded as a PNG."), *Name);
				return false;
			}
			bJpeg = !bTransparent && Bytes.Num() > LargePngBytes;
		}
		if (bJpeg)
		{
			TArray<uint8> Jpeg;
			if (Compress(Image, EImageFormat::JPEG, Jpeg) && (Bytes.Num() == 0 || Jpeg.Num() < Bytes.Num()))
			{
				Bytes = MoveTemp(Jpeg);
			}
			else if (Bytes.Num() == 0)
			{
				OutError = FString::Printf(TEXT("'%s' could not be encoded as a JPEG."), *Name);
				return false;
			}
			else
			{
				bJpeg = false;
			}
		}
		if (Bytes.Num() > CursorImages::MaxEncodedBytes)
		{
			OutError = FString::Printf(TEXT("'%s' is still %.1f MB after shrinking, which is more than can be sent."), *Name, Bytes.Num() / (1024.0 * 1024.0));
			return false;
		}

		Out.MimeType = bJpeg ? TEXT("image/jpeg") : TEXT("image/png");
		Out.Width = Image.SizeX;
		Out.Height = Image.SizeY;
		Out.Name = Name;
		Out.Data = MakeShared<const TArray<uint8>>(MoveTemp(Bytes));
		return true;
	}

	bool Decode(const void* Bytes, int64 NumBytes, const FString& Name, FImage& OutImage, EImageFormat& OutFormat, FString& OutError)
	{
		if (Bytes == nullptr || NumBytes <= 0)
		{
			OutError = FString::Printf(TEXT("'%s' is empty."), *Name);
			return false;
		}
		if (NumBytes > CursorImages::MaxSourceBytes)
		{
			OutError = FString::Printf(TEXT("'%s' is %.0f MB. The largest picture that can be attached is %lld MB."),
				*Name, NumBytes / (1024.0 * 1024.0), CursorImages::MaxSourceBytes / (1024 * 1024));
			return false;
		}
		OutFormat = Wrappers().DetectImageFormat(Bytes, NumBytes);
		if (OutFormat != EImageFormat::PNG && OutFormat != EImageFormat::JPEG && OutFormat != EImageFormat::BMP && OutFormat != EImageFormat::TGA)
		{
			OutError = FString::Printf(TEXT("'%s' is not a picture this can read. Use %s."), *Name, *CursorImages::SupportedFormats());
			return false;
		}
		if (!Wrappers().DecompressImage(Bytes, NumBytes, OutImage) || OutImage.SizeX <= 0 || OutImage.SizeY <= 0)
		{
			OutError = FString::Printf(TEXT("'%s' could not be read. The file may be damaged."), *Name);
			return false;
		}
		if (static_cast<int64>(OutImage.SizeX) * OutImage.SizeY > MaxSourcePixels)
		{
			OutError = FString::Printf(TEXT("'%s' is %d x %d pixels, which is too large to attach."), *Name, OutImage.SizeX, OutImage.SizeY);
			return false;
		}
		OutImage.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		return true;
	}

	bool FromEncodedInternal(const void* Bytes, int64 NumBytes, const FString& Name, bool bKeepAlpha, FCursorImage& Out, FString& OutError)
	{
		FImage Image;
		EImageFormat Format = EImageFormat::Invalid;
		if (!Decode(Bytes, NumBytes, Name, Image, Format, OutError))
		{
			return false;
		}

		// Already in a format the providers take and already small: send the file as it is.
		const bool bSendable = Format == EImageFormat::PNG || Format == EImageFormat::JPEG;
		if (bSendable && bKeepAlpha && FMath::Max(Image.SizeX, Image.SizeY) <= CursorImages::MaxLongEdge && NumBytes <= LargePngBytes)
		{
			TArray<uint8> Copy;
			Copy.Append(static_cast<const uint8*>(Bytes), static_cast<int32>(NumBytes));
			Out.MimeType = Format == EImageFormat::JPEG ? TEXT("image/jpeg") : TEXT("image/png");
			Out.Width = Image.SizeX;
			Out.Height = Image.SizeY;
			Out.Name = Name;
			Out.Data = MakeShared<const TArray<uint8>>(MoveTemp(Copy));
			return true;
		}
		return Encode(Image, Format == EImageFormat::JPEG, bKeepAlpha, Name, Out, OutError);
	}
}

FString FCursorImage::ToBase64() const
{
	return Data.IsValid() ? FBase64::Encode(Data->GetData(), Data->Num()) : FString();
}

FString FCursorImage::Hash() const
{
	if (!Data.IsValid())
	{
		return FString();
	}
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Data->GetData(), Data->Num(), Digest);
	return BytesToHex(Digest, FSHA1::DigestSize).ToLower();
}

const TCHAR* FCursorImage::Extension() const
{
	return MimeType == TEXT("image/jpeg") ? TEXT("jpg") : TEXT("png");
}

FString FCursorImage::Describe() const
{
	const double Kb = NumBytes() / 1024.0;
	const FString Size = Kb >= 1024.0 ? FString::Printf(TEXT("%.1f MB"), Kb / 1024.0) : FString::Printf(TEXT("%.0f KB"), Kb);
	return FString::Printf(TEXT("%s, %d x %d, %s"), Name.IsEmpty() ? TEXT("picture") : *Name, Width, Height, *Size);
}

bool CursorImages::IsImageFile(const FString& Path)
{
	const FString Extension = FPaths::GetExtension(Path).ToLower();
	return Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("bmp") || Extension == TEXT("tga");
}

FString CursorImages::SupportedFormats()
{
	return TEXT("PNG, JPEG, BMP or TGA");
}

bool CursorImages::FromEncoded(const void* Bytes, int64 NumBytes, const FString& Name, FCursorImage& Out, FString& OutError)
{
	return FromEncodedInternal(Bytes, NumBytes, Name, true, Out, OutError);
}

bool CursorImages::FromDib(const void* Bytes, int64 NumBytes, const FString& Name, FCursorImage& Out, FString& OutError)
{
	// BITMAPINFOHEADER, read by offset so this does not depend on the Windows headers.
	constexpr int64 InfoHeaderSize = 40;
	constexpr int64 FileHeaderSize = 14;
	constexpr uint32 BitFields = 3;
	const uint8* Dib = static_cast<const uint8*>(Bytes);
	auto Read32 = [Dib](int64 Offset) { uint32 Value; FMemory::Memcpy(&Value, Dib + Offset, sizeof(Value)); return Value; };
	auto Read16 = [Dib](int64 Offset) { uint16 Value; FMemory::Memcpy(&Value, Dib + Offset, sizeof(Value)); return Value; };

	const FString Unreadable = FString::Printf(TEXT("'%s' is a bitmap in a form this cannot read."), *Name);
	if (Dib == nullptr || NumBytes < InfoHeaderSize || NumBytes > MaxSourceBytes)
	{
		OutError = Unreadable;
		return false;
	}
	const int64 HeaderSize = Read32(0);
	const uint32 BitCount = Read16(14);
	const uint32 Compression = Read32(16);
	const uint32 ColoursUsed = Read32(32);
	if (HeaderSize < InfoHeaderSize || HeaderSize > NumBytes || BitCount == 0 || BitCount > 32)
	{
		OutError = Unreadable;
		return false;
	}
	int64 PixelOffset = HeaderSize;
	if (BitCount <= 8)
	{
		PixelOffset += static_cast<int64>(ColoursUsed != 0 ? ColoursUsed : (1u << BitCount)) * 4;
	}
	else
	{
		PixelOffset += static_cast<int64>(ColoursUsed) * 4;
		// With the oldest header the three colour masks follow it rather than being part of it.
		if (Compression == BitFields && HeaderSize == InfoHeaderSize)
		{
			PixelOffset += 12;
		}
	}
	if (PixelOffset >= NumBytes)
	{
		OutError = Unreadable;
		return false;
	}

	// Put back the 14-byte file header, which makes it a .bmp the image reader understands.
	TArray<uint8> Bmp;
	Bmp.Reserve(static_cast<int32>(FileHeaderSize + NumBytes));
	Bmp.Add('B');
	Bmp.Add('M');
	const uint32 FileSize = static_cast<uint32>(FileHeaderSize + NumBytes);
	const uint32 Reserved = 0;
	const uint32 OffBits = static_cast<uint32>(FileHeaderSize + PixelOffset);
	Bmp.Append(reinterpret_cast<const uint8*>(&FileSize), 4);
	Bmp.Append(reinterpret_cast<const uint8*>(&Reserved), 4);
	Bmp.Append(reinterpret_cast<const uint8*>(&OffBits), 4);
	Bmp.Append(Dib, static_cast<int32>(NumBytes));
	return FromEncodedInternal(Bmp.GetData(), Bmp.Num(), Name, false, Out, OutError);
}

bool CursorImages::FromFile(const FString& Path, FCursorImage& Out, FString& OutError)
{
	const FString Name = FPaths::GetCleanFilename(Path);
	if (!IsImageFile(Path))
	{
		OutError = FString::Printf(TEXT("'%s' is not a picture this can read. Use %s."), *Name, *SupportedFormats());
		return false;
	}
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size < 0)
	{
		OutError = FString::Printf(TEXT("'%s' could not be found."), *Name);
		return false;
	}
	if (Size > MaxSourceBytes)
	{
		OutError = FString::Printf(TEXT("'%s' is %.0f MB. The largest picture that can be attached is %lld MB."), *Name, Size / (1024.0 * 1024.0), MaxSourceBytes / (1024 * 1024));
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		OutError = FString::Printf(TEXT("'%s' could not be opened."), *Name);
		return false;
	}
	return FromEncoded(Bytes.GetData(), Bytes.Num(), Name, Out, OutError);
}

bool CursorImages::FromPixels(const TArray<FColor>& Pixels, int32 Width, int32 Height, const FString& Name, bool bPhoto, FCursorImage& Out, FString& OutError)
{
	if (Width <= 0 || Height <= 0 || static_cast<int64>(Width) * Height != Pixels.Num())
	{
		OutError = FString::Printf(TEXT("'%s' has no pixels."), *Name);
		return false;
	}
	FImage Image(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), static_cast<SIZE_T>(Pixels.Num()) * sizeof(FColor));
	return Encode(Image, bPhoto, false, Name, Out, OutError);
}

bool CursorImages::DecodeForDisplay(const FCursorImage& Image, int32 MaxEdge, TArray<uint8>& OutBgra, int32& OutWidth, int32& OutHeight)
{
	if (!Image.IsValid() || MaxEdge <= 0)
	{
		return false;
	}
	FImage Decoded;
	if (!Wrappers().DecompressImage(Image.Data->GetData(), Image.Data->Num(), Decoded) || Decoded.SizeX <= 0 || Decoded.SizeY <= 0)
	{
		return false;
	}
	Decoded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	const int32 LongEdge = FMath::Max(Decoded.SizeX, Decoded.SizeY);
	if (LongEdge > MaxEdge)
	{
		const double Scale = static_cast<double>(MaxEdge) / LongEdge;
		FImageCore::ResizeImageInPlace(Decoded, FMath::Max(1, FMath::RoundToInt32(Decoded.SizeX * Scale)), FMath::Max(1, FMath::RoundToInt32(Decoded.SizeY * Scale)),
			ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	}
	OutWidth = Decoded.SizeX;
	OutHeight = Decoded.SizeY;
	OutBgra.SetNumUninitialized(static_cast<int32>(Decoded.RawData.Num()));
	FMemory::Memcpy(OutBgra.GetData(), Decoded.RawData.GetData(), Decoded.RawData.Num());
	return true;
}

FString CursorImages::GetAttachmentDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("Attachments"));
}

FString CursorImages::SaveAttachment(const FCursorImage& Image)
{
	if (!Image.IsValid())
	{
		return FString();
	}
	// Named by content, so the same picture sent twice is stored once.
	const FString Path = GetAttachmentDirectory() / FString::Printf(TEXT("%s.%s"), *Image.Hash().Left(20), Image.Extension());
	if (IFileManager::Get().FileExists(*Path))
	{
		return Path;
	}
	IFileManager::Get().MakeDirectory(*GetAttachmentDirectory(), true);
	if (!FFileHelper::SaveArrayToFile(*Image.Data, *Path))
	{
		UE_LOG(LogCursorUnreal, Warning, TEXT("A picture could not be saved to the attachment folder."));
		return FString();
	}
	return Path;
}

#if PLATFORM_WINDOWS

namespace
{
	/** Copies a clipboard memory block. Empty when the format is not on the clipboard. */
	TArray<uint8> ClipboardBytes(uint32 Format)
	{
		TArray<uint8> Out;
		if (!::IsClipboardFormatAvailable(Format))
		{
			return Out;
		}
		const HANDLE Handle = ::GetClipboardData(Format);
		if (Handle == nullptr)
		{
			return Out;
		}
		const SIZE_T Size = ::GlobalSize(Handle);
		if (Size == 0 || Size > static_cast<SIZE_T>(CursorImages::MaxSourceBytes))
		{
			return Out;
		}
		if (const void* Locked = ::GlobalLock(Handle))
		{
			Out.Append(static_cast<const uint8*>(Locked), static_cast<int32>(Size));
			::GlobalUnlock(Handle);
		}
		return Out;
	}

}

bool CursorImages::ReadClipboard(TArray<FCursorImage>& OutImages, TArray<FString>& OutErrors)
{
	// Copying from a word processor or spreadsheet puts a rendering of the text on the clipboard next to the text. That is a text paste.
	if (::IsClipboardFormatAvailable(CF_UNICODETEXT) || ::IsClipboardFormatAvailable(CF_TEXT))
	{
		return false;
	}
	if (!::OpenClipboard(nullptr))
	{
		return false;
	}

	TArray<uint8> Png = ClipboardBytes(::RegisterClipboardFormatW(L"PNG"));
	TArray<uint8> Dib;
	TArray<FString> Files;
	if (Png.Num() == 0)
	{
		if (::IsClipboardFormatAvailable(CF_HDROP))
		{
			if (const HDROP Drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP)))
			{
				const UINT Count = ::DragQueryFileW(Drop, 0xFFFFFFFF, nullptr, 0);
				for (UINT Index = 0; Index < Count; ++Index)
				{
					WCHAR Path[1024];
					if (::DragQueryFileW(Drop, Index, Path, UE_ARRAY_COUNT(Path)) > 0)
					{
						Files.Add(FString(Path));
					}
				}
			}
		}
		else
		{
			Dib = ClipboardBytes(CF_DIB);
		}
	}
	::CloseClipboard();

	FString Error;
	if (Png.Num() > 0)
	{
		FCursorImage Image;
		if (FromEncoded(Png.GetData(), Png.Num(), TEXT("pasted picture"), Image, Error))
		{
			OutImages.Add(MoveTemp(Image));
		}
		else
		{
			OutErrors.Add(Error);
		}
		return true;
	}
	if (Files.Num() > 0)
	{
		bool bAnyPicture = false;
		for (const FString& File : Files)
		{
			if (!IsImageFile(File))
			{
				continue;
			}
			bAnyPicture = true;
			FCursorImage Image;
			if (FromFile(File, Image, Error))
			{
				OutImages.Add(MoveTemp(Image));
			}
			else
			{
				OutErrors.Add(Error);
			}
		}
		return bAnyPicture;
	}
	if (Dib.Num() > 0)
	{
		FCursorImage Image;
		if (FromDib(Dib.GetData(), Dib.Num(), TEXT("pasted picture"), Image, Error))
		{
			OutImages.Add(MoveTemp(Image));
		}
		else
		{
			OutErrors.Add(Error);
		}
		return true;
	}
	return false;
}

#else

bool CursorImages::ReadClipboard(TArray<FCursorImage>& OutImages, TArray<FString>& OutErrors)
{
	return false;
}

#endif
