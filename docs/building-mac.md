## Build instructions for macOS

### Prepare folder

Choose a folder for the future build, for example **/Users/user/TBuild**. It will be named ***BuildPath*** in the rest of this document. All commands will be launched from Terminal.

**Note about disk space:** The full build process will require approximately **55 GB** of free space. This includes:
- **~35 GB** for libraries (when building for both x64 and arm64 architectures)
- **~20 GB** for the compiled app (in the `out` folder)

### Obtain your API credentials

You will require **api_id** and **api_hash** to access the API servers. To learn how to obtain them [click here][api_credentials].

### Clone source code and prepare libraries

Go to ***BuildPath*** and run

    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    brew install git automake libtool cmake wget pkg-config gnu-tar ninja nasm meson

    sudo xcode-select -s /Applications/Xcode.app/Contents/Developer

    git clone --recursive https://github.com/Impulse-m/ImpulseM.Client.Desktop.git tdesktop
    ./tdesktop/Telegram/build/prepare/mac.sh

### Building the project

Go to ***BuildPath*/tdesktop/Telegram** and run (using [your **api_id** and **api_hash**](#obtain-your-api-credentials))

    ./configure.sh -D TDESKTOP_API_ID=YOUR_API_ID -D TDESKTOP_API_HASH=YOUR_API_HASH -D IMPULSEM_RPC_URL=YOUR_RPC_URL

Replace `YOUR_RPC_URL` with the base URL of the transport host. The other build variables are listed in the [build variables](../README.md#build-variables) section of the README.

Then launch Xcode, open ***BuildPath*/tdesktop/out/Telegram.xcodeproj** and build for Debug / Release.

[api_credentials]: api_credentials.md
