# PocketBook OPDS Client

An OPDS catalog client built in C for PocketBook e-readers. This application allows users to connect to OPDS servers (such as COPS, Calibre-Web, and Project Gutenberg), browse catalogs, search for titles, and download books directly to the device.

## Features

* **E-Ink Optimized UI:** Built with `libinkview`. Includes visual touch feedback (screen inversion) for button presses and interactions.
* **Native Dark Mode Support:** Seamlessly integrates with PocketBook's system-wide Dark Mode (Firmware 6.8+). Book covers are intelligently processed so they retain their original colors while the rest of the interface inverts perfectly.
* **Customizable Layout:** Adjust the catalog view to display between 4 and 10 rows per page via the Server Settings menu. Fonts, thumbnails, and touch targets scale dynamically to ensure comfortable reading and tapping at any size.
* **Robust Configuration:** Settings are safely stored in a text-based file directly in the app's folder (`/mnt/ext1/applications/OPDSClient/opds_client.cfg`) to prevent system write-protection issues. The app automatically detects and migrates legacy binary save files.
* **Embedded Assets:** Icons are compiled directly into the binary, requiring no external image files for the base UI. Line-art icons smoothly adapt to Dark Mode.
* **Cache Management:** Maintains a 20MB cache limit for downloaded cover images. The app automatically deletes the oldest thumbnails when this limit is reached.
* **Search Support:** Compatible with standard OPDS search endpoints and OpenSearch. Includes URL encoding to handle multi-word searches on Python-based servers like Calibre-Web.
* **Continuous Pagination:** Tracks page numbers across server batches to provide a seamless browsing experience.
* **Network Handling:** Uses connection reuse, automatically resolves relative URLs, and implements 30-second timeouts to handle unresponsive servers without freezing the device.
* **Downloads:** Books are downloaded directly to the device's storage. 
  * *Note on Library Integration:* Books downloaded via the app may not automatically appear in the native PocketBook Library app. You may need to trigger a library rescan or use an external script (such as an `iv2sh` command) to force the OS to index the new files.

---

## Installation

1. Download `OPDSClient.tar.gz` from the latest GitHub release and extract `OPDSClient.app`.
2. Connect your PocketBook to your computer via USB.
3. Copy `OPDSClient.app` into the `applications` folder on your device's internal storage. *(Note: This folder may be hidden by your computer's operating system).*
4. OPTIONAL:  If you want to have a couple of starting servers aleady configured you can copy the `opds_client.cfg` file into the `applications\OPDSClient\` folder.  If you havn't run the application yet the `OPDSClient` folder won't exist. You will have to create it manually or run the app once and come back and copy this file.
5. Disconnect the device. The app will be available in your PocketBook's "Applications" menu.

---

## Navigation & Controls

The application supports both touchscreen input and hardware button navigation.

### Touchscreen Controls
* **Visual Feedback:** Tapping a book or folder will invert the row color to indicate the touch was registered while the network request processes.
* **Header Navigation:** When browsing a catalog, tap the catalog title at the top of the screen to return directly to the Server Options menu.

### Hardware Button Mappings

| Button | Context: Catalog Browsing | Context: Book Details |
| :--- | :--- | :--- |
| **Next Page** | Load the next page of the catalog. | Scroll down the book summary text. |
| **Prev Page** | Load the previous page. | Scroll up the book summary text. |
| **Menu / Home**| Opens a number pad allowing you to jump to a specific page. Supports "Time Machine" jumping back to previously loaded batches. | *No action* |

---

## Debug Logging

The app logs network requests and errors if a trigger file is present.

**To enable and access logs:**
1. Create an empty file named `LOGTRIGGER.TXT` in the app's installation folder (`/mnt/ext1/applications/OPDSClient/`).
2. Run the application and perform the actions you wish to log.
3. Open the `opds_client.log` file on your computer. This file contains `libcurl` network traces, HTTP headers, and redirect information.

I have tested on a Pocketbook ERA and Inkpad Color 3. I made the UI scalable but have not tested on any older or lower resolution devices.

---

## Compiling from Source

This application targets PocketBook SDK 6.8 with the B300 architecture.

The recommended build environment is the `andi97/pocketbook-dev-docker:6.8-b300-r1` Docker image.

Dependencies required to compile:
* `libinkview` (PocketBook UI)
* `libcurl` (Networking)
* `libxml2` (OPDS Parsing)
* `freetype2` (Font Rendering)
* `stb_image.h` (Included header for decoding images)

To compile from the repository root, run:
```bash
docker pull andi97/pocketbook-dev-docker:6.8-b300-r1

docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$PWD:/project" \
  -w /project \
  andi97/pocketbook-dev-docker:6.8-b300-r1 \
  -lc 'export LD_LIBRARY_PATH="$SDK_BASE/usr/lib"; \
       make clean && \
       make -j"$(nproc)" SDK_PATH="$SDK_BASE/usr"'
```

The resulting application is `OPDSClient.app`.

## Automated Releases

Every update to the `main` branch is built with the same SDK image by GitHub Actions. A successful build creates a GitHub release with autogenerated release notes and attaches `OPDSClient.tar.gz`, containing the installable `OPDSClient.app`.

Release tags use the UTC build date and time followed by the short commit hash, for example `build-20260727-174854-a1b2c3d`.

## Customizing Icons (Advanced/Optional)

Note: You do not need to do this to use or compile the app. The default icons are already converted and safely stored inside the `icons.h` file.

If you want to replace the default folder or book icons with your own custom images, a Python helper script is included in the source code.

1. Ensure you have Python 3 installed on your computer.
2. Replace `folder.png` and `book.png` in the project directory with your own standard, 24-bit RGB PNG files. (The app will automatically calculate the aspect ratio for the folder icon, so any standard dimensions will work).
3. Run the conversion script from your terminal:
   ```bash
   python3 image_to_c.py
   ```
    
This will instantly overwrite the `icons.h` file, converting your new images into raw C byte arrays.

Recompile the app using `make`. Your new icons are now permanently embedded in the final `.app` file!

## License

This project is licensed under the MIT License. See the LICENSE file for details.

## Acknowledgments

Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org). All rights reserved.
