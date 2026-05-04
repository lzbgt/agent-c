# DeepSeek API model selection for agentd

Date: 2026-05-04

## Sources

- DeepSeek API Docs, "Your First API Call": https://api-docs.deepseek.com/
- DeepSeek API Docs, "Models & Pricing": https://api-docs.deepseek.com/quick_start/pricing
- DeepSeek API Docs, "Lists Models": https://api-docs.deepseek.com/api/list-models/
- DeepSeek API Docs, "DeepSeek V4 Preview Release": https://api-docs.deepseek.com/news/news260424

## Decision

Use `deepseek-v4-pro` as the default hosted model for durable local `agentd` instances installed by the repo launchd installer. Use `https://api.deepseek.com` as the OpenAI-compatible base URL, and load `DEEPSEEK_API_KEY` from the process environment or from the default dotenv path.

The official DeepSeek docs currently list `deepseek-v4-pro` and `deepseek-v4-flash` as API model identifiers. The docs also state that `deepseek-chat` and `deepseek-reasoner` are compatibility aliases that will be deprecated on 2026-07-24, so those older names should not be used as the project default.

## Local proof

The local provider diagnostics endpoint was tested against the user environment. `deepseek-v4-pro` returned a successful assistant response using the configured `DEEPSEEK_API_KEY`, while the existing durable daemon configuration was still pointed at OpenAI without an OpenAI key. That mismatch explained why previously accepted agentd workflow submits could remain queued/running or end with provider errors without producing assistant messages.

The launchd installer now defaults to:

```text
AGENTD_DOTENV_PATH=$HOME/.env
--base-url https://api.deepseek.com
--model deepseek-v4-pro
--timeout-ms 60000
```

Secrets remain in environment variables or dotenv files. They are not written into launchd `ProgramArguments`.
