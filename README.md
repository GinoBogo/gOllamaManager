# gOllamaManager

A terminal-based user interface (TUI) for managing Ollama models. Built with C and ncurses, gOllamaManager provides an intuitive console interface to view, pull, delete, and manage your Ollama models. This tool was created specifically to manage LLM models on a remote Linux server running Ollama by means of SSH tunneling.

## Features

- **View Installed Models**: Browse all installed Ollama models with details (name, ID, size, modification date)
- **View Running Models**: Monitor currently running models with processor usage and expiration time
- **Pull Models**: Download new models from the Ollama registry
- **Delete Models**: Remove installed models with confirmation
- **Stop Models**: Stop running models with confirmation
- **Model Information**: View detailed information about any model
- **Search/Filter**: Quickly find models by name
- **Background Refresh**: Automatic data refresh in a background thread
- **Color-coded UI**: Easy-to-read interface with color highlighting

![Figure 01](./docs/images/Figure_01.png)

![Figure 02](./docs/images/Figure_02.png)

![Figure 03](./docs/images/Figure_03.png)

![Figure 04](./docs/images/Figure_04.png)

![Figure 05](./docs/images/Figure_05.png)

![Figure 06](./docs/images/Figure_06.png)

## Requirements

- Linux operating system
- GCC compiler
- CMake (3.10 or higher)
- ncurses library
- pthread library
- Ollama installed and available in PATH

## Installation

### Clone the Repository

```bash
git clone <repository-url>
cd gOllamaManager
```

### Build the Project

Using the provided build script:

```bash
./build.sh release
```

Or manually with CMake:

```bash
mkdir build
cd build
cmake ..
make
```

The executable will be located at `build/bin/gollama_manager`.

### Optional: Install System-wide

```bash
sudo cp build/bin/gollama_manager /usr/local/bin/
```

## Usage

Run the application:

```bash
./build/bin/gollama_manager
```

Or if installed system-wide:

```bash
gollama_manager
```

## Key Bindings

### Navigation
- **UP/DOWN** - Navigate through the model list
- **LEFT/RIGHT** - Switch between "Installed Models" and "Running Models" tabs

### Model Operations
- **I** - Show detailed information about the selected model
- **D** - Delete the selected model (with confirmation)
- **S** - Stop the selected running model (with confirmation)
- **P** - Pull a new model (opens input dialog)

### Other
- **R** - Refresh model data
- **/** - Open search dialog to filter models
- **Q** - Quit the application

### Dialog Controls
- **ENTER** - Confirm action / submit input
- **ESC** - Cancel dialog
- **TAB** - Toggle between OK/Cancel in confirmation dialogs
- **LEFT/RIGHT** - Navigate between OK/Cancel in confirmation dialogs

## Building from Source

### Debug Build

```bash
./build.sh debug
```

### Release Build

```bash
./build.sh release
```

### Clean Build Artifacts

```bash
./clean.sh
```

### Full Rebuild

```bash
./rebuild.sh release
```

## Technical Details

- **Language**: C
- **UI Framework**: ncurses
- **Threading**: pthread
- **Build System**: CMake
- **Compiler**: GCC with strict warnings enabled

## License

MIT License

## Author

Gino Francesco Bogo (ᛊᛟᚱᚱᛖ ᛗᛖᚨ ᛁᛊᛏᚨᛗᛁ ᚨcᚢᚱᛉᚢ)

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.

## Troubleshooting

### Ollama not found
Ensure Ollama is installed and available in your PATH:
```bash
ollama --version
```

### ncurses not found
Install ncurses development library:
```bash
# Arch Linux
sudo pacman -S ncurses

# Ubuntu/Debian
sudo apt-get install libncurses-dev

# Fedora/RHEL
sudo dnf install ncurses-devel
```

### Build errors
Ensure you have GCC and CMake installed:
```bash
gcc --version
cmake --version
```
