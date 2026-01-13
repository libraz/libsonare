# Contributing to libsonare

Thank you for your interest in contributing to libsonare! We welcome contributions from the community.

## How to Contribute

1. **Fork the repository**
2. **Create a feature branch**
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **Make your changes**
4. **Test your changes**
   ```bash
   make test
   ```
5. **Commit your changes**
   ```bash
   git commit -m "feat: your feature description"
   ```
6. **Push to your fork**
   ```bash
   git push origin feature/your-feature-name
   ```
7. **Open a Pull Request**

## Pull Request Guidelines

- **Tests**: Ensure all tests pass (`make test`)
- **Description**: Clearly describe what your PR does and why
- **One feature per PR**: Keep changes focused and atomic

## Code Quality

**Only requirement: Tests must pass**

```bash
make test  # This is the only required check
```

CI also runs formatting checks, but these are **not blocking**:
- **Formatting** (clang-format): Runs in CI, warnings only

If there are style issues, the maintainer will fix them. You don't need to worry about formatting.

## Development Setup

**Quick start:**
```bash
# Build project
make build

# Run tests
make test

# Format code (optional)
make format
```

For WebAssembly build:
```bash
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --parallel
```

## Coding Notes

- C++17 (no C++20 features)
- Comments in English
- Match librosa defaults where applicable (sr=22050, n_fft=2048, hop_length=512)

## Reporting Issues

- Check if the issue already exists
- Provide clear reproduction steps
- Include relevant logs and system information

## Questions?

Feel free to open an issue for questions or discussions.

## License

By contributing, you agree that your contributions will be licensed under the Apache License 2.0.
