#pragma once

#include "CoreMinimal.h"

/**
 * A picture on its way to or from a model: a reference image the user attached, or a screenshot a
 * tool took. Holds the encoded file (PNG or JPEG), never raw pixels.
 */
struct CURSORUNREALCORE_API FCursorImage
{
	/** "image/png" or "image/jpeg". */
	FString MimeType;
	/** The encoded file. Shared: the chat, the request and the transcript all hold the same picture. */
	TSharedPtr<const TArray<uint8>> Data;
	int32 Width = 0;
	int32 Height = 0;
	/** Where it came from, for people: a file name, "clipboard", "viewport". */
	FString Name;

	bool IsValid() const { return Data.IsValid() && Data->Num() > 0 && !MimeType.IsEmpty(); }
	int64 NumBytes() const { return Data.IsValid() ? Data->Num() : 0; }
	FString ToBase64() const;
	/** Hex SHA-1 of the file. Identifies the picture in a transcript without the picture being in it. */
	FString Hash() const;
	/** "png" or "jpg". */
	const TCHAR* Extension() const;
	/** "wall.png, 1280x720, 212 KB". */
	FString Describe() const;
};

namespace CursorImages
{
	/** Pictures are shrunk to this on their longer side before they are sent. Larger adds cost, not detail the models use. */
	constexpr int32 MaxLongEdge = 1568;
	/** Most pictures one message may carry. */
	constexpr int32 MaxPerMessage = 8;
	/** Most bytes of pictures one message may carry, all together. */
	constexpr int64 MaxMessageBytes = 10ll * 1024 * 1024;
	/** Largest file read from disk or the clipboard. */
	constexpr int64 MaxSourceBytes = 64ll * 1024 * 1024;
	/** Largest encoded picture sent. Providers refuse more than about 5 MB. */
	constexpr int64 MaxEncodedBytes = 4ll * 1024 * 1024;

	/** True for a file name the functions below can read: png, jpg, jpeg, bmp, tga. */
	CURSORUNREALCORE_API bool IsImageFile(const FString& Path);
	/** What IsImageFile accepts, for messages. */
	CURSORUNREALCORE_API FString SupportedFormats();

	/**
	 * Reads an encoded picture, shrinks it if it is larger than MaxLongEdge, and encodes it for
	 * sending. A PNG or JPEG that is already small enough is passed through untouched.
	 */
	CURSORUNREALCORE_API bool FromEncoded(const void* Bytes, int64 NumBytes, const FString& Name, FCursorImage& Out, FString& OutError);
	CURSORUNREALCORE_API bool FromFile(const FString& Path, FCursorImage& Out, FString& OutError);
	/**
	 * From a device-independent bitmap, the form a copied picture takes on the Windows clipboard:
	 * a .bmp file without its file header. Its fourth byte per pixel is ignored, because
	 * screenshots leave it at zero and read as transparency that would make them invisible.
	 */
	CURSORUNREALCORE_API bool FromDib(const void* Bytes, int64 NumBytes, const FString& Name, FCursorImage& Out, FString& OutError);
	/**
	 * From pixels in memory. Alpha is ignored. bPhoto picks JPEG, which suits renders; otherwise
	 * PNG, which keeps text and hard edges sharp.
	 */
	CURSORUNREALCORE_API bool FromPixels(const TArray<FColor>& Pixels, int32 Width, int32 Height, const FString& Name, bool bPhoto, FCursorImage& Out, FString& OutError);

	/** Decodes to BGRA8 pixels no larger than MaxEdge on the longer side, for showing in the chat. */
	CURSORUNREALCORE_API bool DecodeForDisplay(const FCursorImage& Image, int32 MaxEdge, TArray<uint8>& OutBgra, int32& OutWidth, int32& OutHeight);

	/** Saved/CursorUnreal/Attachments. Under Saved, so it is not shared through source control. */
	CURSORUNREALCORE_API FString GetAttachmentDirectory();
	/** Writes the picture into the attachment folder under a name made from its hash. Returns the path, or empty when it could not be written. */
	CURSORUNREALCORE_API FString SaveAttachment(const FCursorImage& Image);

	/**
	 * Takes pictures off the system clipboard: a copied image (a screenshot, "Copy image" in a
	 * browser) or copied image files. Returns false when the clipboard holds neither, so the
	 * caller can let an ordinary text paste go ahead. OutErrors lists pictures that were there
	 * but could not be used. Windows only; elsewhere it returns false.
	 */
	CURSORUNREALCORE_API bool ReadClipboard(TArray<FCursorImage>& OutImages, TArray<FString>& OutErrors);
}
