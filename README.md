# onekvm-system-lifecycle

English | [简体中文](README.zh-CN.md)

Platform-independent system lifecycle support for OneKVM. The repository
currently provides persistent configuration, extension state, package state,
kernel-module updates, and storage preparation used across OneKVM targets.

Machine-specific partition creation, first-boot migration, and hardware health
checks remain in each machine support repository.

The OpenEmbedded recipe remains in the `meta-onekvm` layer.

## License

MIT. See [LICENSE](LICENSE).
