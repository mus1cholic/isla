# isla

C++ desktop-mate framework in progress (Bazel + GoogleTest).

## Build Executable

From the repo root:

```bash
bazel build //client/src:isla_client
```

Built binary (Windows):

```text
bazel-bin/client/src/isla_client.exe
```

Run directly through Bazel:

```bash
bazel run //client/src:isla_client
```
