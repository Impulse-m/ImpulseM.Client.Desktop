# ImpulseM Desktop

ImpulseM Desktop is the desktop client of the ImpulseM platform for Windows, macOS and Linux.

## Build

Build instructions for each platform are in the guides:

* [Windows (32-bit and 64-bit)](docs/building-win.md)
* [macOS](docs/building-mac.md)
* [GNU/Linux using Docker](docs/building-linux.md)

The steps are the same on every platform:

1. Get your **api_id** and **api_hash**, see [docs/api_credentials.md](docs/api_credentials.md).
2. Clone the repository with `--recursive` and run the prepare script for your platform from `Telegram/build/prepare`.
3. Configure the build with `TDESKTOP_API_ID`, `TDESKTOP_API_HASH` and the build variables below.
4. Build the app with the toolchain of your platform.

## Build variables

Pass each variable as a CMake option, for example `-D IMPULSEM_RPC_URL=https://rpc.example.org`, or set an environment variable with the same name. CI reads them from the GitHub variables with the same names.

| Variable | What it does | When it is empty |
|----------|--------------|------------------|
| `IMPULSEM_RPC_URL` | Base URL of the transport host, for example `https://rpc.example.org`. The app derives its WebSocket endpoints from it. | CI app builds fail with an error. A local build makes no connection and the app stays not connected. |
| `IMPULSEM_SITE_URL` | Base URL of the project website, without a trailing slash, for example `https://example.org`. Links to its pages and downloads are built from it. | Website menu entries and buttons are hidden, and text that linked to the website is shown without a link. |
| `IMPULSEM_LINK_HOST` | Host of public share links, without a scheme, for example `o.example.org`. | No host is recognised statically. Links rely on the prefix sent by the server and on the app's URL scheme, and UI that needs a host to build a link is hidden. |
| `IMPULSEM_WEB_URL` | URL of the web client, for example `https://web.example.org/k`. | "Open the web version" links are hidden. |

## License

The source code is published under the GPLv3 license with OpenSSL exception. See [LICENSE](LICENSE). Changes to the upstream project are described in [LEGAL](LEGAL), and the upstream copyright notices are kept.
