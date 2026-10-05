"""Offline conversion: python decode.py path/to/STBN.zip (requires Pillow)."""
import hashlib
import io
from pathlib import Path
import sys
import zipfile
from PIL import Image

ARCHIVE_SHA256 = "f262aaa79704b913ad1ac22b11674931c5c16a788688f8ce49ec43d59eb5c747"
OUTPUT_SHA256 = "b3d8b7e55f1cb89683d1a4f51c494fe8071a81a36875eecbdc00f27add5ec0b1"


def main():
    archive_bytes = Path(sys.argv[1]).read_bytes()
    if hashlib.sha256(archive_bytes).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("Expected the STBN.zip revision recorded in README.md")
    output = bytearray()
    with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
        for frame in range(64):
            name = f"STBN/stbn_vec2_2Dx1D_128x128x64_{frame}.png"
            with Image.open(archive.open(name)) as image:
                if image.size != (128, 128) or image.mode != "RGBA":
                    raise ValueError(f"Unexpected image format: {name}")
                # Preserve original byte values and row order; discard unused B/A.
                output.extend(Image.merge("LA", image.split()[:2]).tobytes())
    if hashlib.sha256(output).hexdigest() != OUTPUT_SHA256:
        raise ValueError("Decoded STBN data checksum mismatch")
    Path(__file__).with_name("stbn_vec2_128x128x64.rg8").write_bytes(output)
    print(f"Decoded {len(output)} bytes; SHA-256 {OUTPUT_SHA256}")


if __name__ == "__main__":
    main()
