# Chuan · Android application

[简体中文](README.md) | English

The Android application sends and receives files and folders over a local network. It provides device discovery and a Web service independently. Current version: **V2.3.0**.

See the [project README](../README.en.md) for packages and general usage instructions. Android 8.0 or later and a working system WebView are required.

## Usage

1. Install the APK, open the application, and select a destination device to send files or a folder. Files can also be shared from other applications by selecting “传” in the system share menu.
2. The receiver can enable automatic reception or select pending items and choose “Receive” (接收) or “Reject” (拒绝).
3. Use “Receive directory → Change” (接收目录 → 修改) to grant access to a destination directory. Received files can be browsed, opened, or shared within the application.
4. Copy the access URL from “Web service” (Web 服务) to connect from a browser on the same local network.

Folders are copied with their original directory structure by default. ZIP packaging can be enabled in the sending settings. The task list provides progress, individual results, and controls to pause, resume, and retry transfers.

Opening the application starts a foreground service that continues sending and receiving files while the application is in the background. The service can be stopped from “Web service” (Web 服务) or the notification area.

The default destination is an application-specific directory whose contents are removed when the application is uninstalled. Files that need to be retained should be saved to another directory with access permission. Deleting a receive record also deletes the files received by that task; “Clear completed” (清除已结束) only hides the records.
