# Local LLM Setup

The AI gateway now supports a local Ollama-backed `LlmClient` in addition to the existing OpenAI
Responses upstream.

## What This Enables

- Running the gateway without an OpenAI API key when you want a fully local main LLM.
- Reusing the existing provider-neutral `LlmClient` path for normal turn generation and the
  mid-term memory flush/compaction helpers.
- Keeping the OpenAI-specific Responses tool loop available only when OpenAI is also configured.

## Ollama Configuration

You can configure Ollama with either CLI flags or environment variables.

### CLI

```text
--ollama-base-url=http://127.0.0.1:11434
--ollama-timeout-ms=60000
--main-llm-model=qwen3:4b
```

### Environment

```text
OLLAMA_BASE_URL=http://127.0.0.1:11434
OLLAMA_TIMEOUT_MS=60000
AI_GATEWAY_MAIN_LLM_MODEL=qwen3:4b
```

`OLLAMA_API_KEY` / `--ollama-api-key` is optional and only needed if your Ollama-compatible
endpoint sits behind auth.

## Notes

- The current Ollama client uses `/api/chat`.
- The current implementation returns the final assistant text through the gateway's streaming
  interface as one aggregated text delta, then emits completion.
- If both OpenAI and Ollama are configured, the gateway prefers Ollama for the generic `LlmClient`
  path while still using OpenAI for the Responses-native tool loop.
- Use the exact local model tag exposed by your runtime. For Ollama this is typically a tag such
  as `qwen3:4b`, but your installed tag may differ.
