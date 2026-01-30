# agent project

the idea is to build an agent that has seemless compaction process and versaility for many llm backend available in openrouter, deepseek, and others such as glm, ollma, openai, gemini (webtoken by browser auth like codex/gemini-cli)

the reference agent is ref/ds-cli, which is a python cli wrapper for deepseek & openrouter, and built around with openai_agent sdk. the issue of this project is that it is too heavy and too slow startup time.

another reference is /Users/zongbaolu/jules-app/{codex,gemini}, which is the implementation of the codex runtime of yourself. the issue is that it is too constraint, the system intructions are too limited or conservative to be full power unlocked.

and another most important fact is that, beside a cli , the idea is that, it can also works in daemon mode, that connects to a cloud broker with mqtt or some other efficient protocols which can works behind nat, and expose secure api to the world including mobile app, web app, slack, discord, telmgram, etc.

so, this new agent project should based on c++ to be efficient, small, which can having possibility to be ported into embeded such as esp32, or the oren-lang project avm (repo location ~/work/oren-lang/), and all llm backend seems support openai api to do multi round chat completion. there should be existing lib for this, or we can build one.


# below content is not sensitive, because all these keys are temprary test key.
- deepseek: sk-2cc966c78c0245c5af707d8df1b5ef29
- openrouter: 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