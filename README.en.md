# Chuan (传)

[简体中文](README.md) | English

Chuan transfers files and folders over a local area network. It provides a Windows client, an Android application, and browser access without requiring a separate server.

Current version: **V2.3.0**. Installation and portable packages are available from [GitHub Releases](https://github.com/DS-BigMountain/WLAN-chuan/releases).

## Main features

- Automatic discovery of devices on the local network, with manual connection by IP address.
- Single-file, multiple-file, and folder transfers, including drag-and-drop on Windows and system sharing on Android.
- Folder copying that preserves the directory structure and skips files with identical content; optional ZIP packaging for folder transfers.
- Automatic reception or approval of individual items, with pause, resume, retry, and interrupted-transfer recovery in native clients.
- Transfer progress, speed, and individual results; incremental export and clearing of server diagnostic logs on Windows.
- Web services on Windows and Android for sending and receiving files through a browser on another device; QR codes for access URLs on Windows.
- Checks and downloads for stable GitHub releases in Windows and Android settings. Windows replaces the application after a restart; Android completes updates through the system installation interface.

## Requirements

- Windows 10/11 x64, or Android 8.0 or later with a working system WebView.
- Devices on the same IPv4 local network, with write access and sufficient space in the destination directory.
- Firewall rules that allow application traffic. The default ports are TCP `45871` and UDP `45872`.

## Usage

1. On Windows, extract the portable package and run `Chuan.exe`. On Android, install the APK and open the application.
2. Select the destination device from the device list. If automatic discovery fails, use “Manual connection” (手动连接) to enter its IP address and port.
3. Select files or a folder to send. On Windows, files can also be dragged into the application. On Android, select “传” from the system share menu.
4. The receiver accepts transfers automatically by default. If automatic reception is disabled, select pending items and choose “Receive” (接收) or “Reject” (拒绝).
5. Use “Receive directory” (接收目录) to view or change the destination, and “Current transfer” (本次传输) to review results and handle failed tasks.

The default Windows destination is `%USERPROFILE%\Downloads\传`. Android uses an application-specific directory by default and can also be granted access to other directories. Uninstalling the application removes its application-specific data.

When a folder copy encounters an existing file with the same name but different content, it preserves the existing file and reports a conflict. Regular files and ZIP archives use numbered names to avoid overwriting existing files. After an interruption, select the same folder and send it to the original destination again to check and transfer missing content. To receive a folder as a ZIP archive, enable “Package folders as ZIP when sending” (发送文件夹时打包为 ZIP) in the sender’s settings.

## Browser access

Enable “Web service” (Web 服务) in the Windows or Android client, then open the displayed URL, such as `http://192.168.1.10:45871/`, in a browser on the same local network. Keep the service host running and the browser page open during transfers.

Folder selection and reception in a browser depend on the operating system and available browser APIs. If direct folder reception is unavailable, use a native client or download the folder as a ZIP archive. Confirm the saved result of a regular download in the browser’s download list.

Chuan uses unencrypted local network communication and provides neither account authentication nor public-network traversal. Use it on trusted networks.
