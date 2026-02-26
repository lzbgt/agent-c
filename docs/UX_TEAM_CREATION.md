# UX: Multi-Agent Team Creation

## Goals (User-Centered)
- Let users form a multi-agent team in under 60 seconds.
- Make provider/model selection obvious without exposing credentials in the flow.
- Keep the conversation view clean; separate team setup from run execution.
- Support both fast defaults and deep control without clutter.

## Primary User Story
As a user, I want to create a team of agents with different LLM providers, assign roles,
and run a task without being forced to understand every system detail.

## Information Architecture
- Top-level: **Teams**
  - **Team List**
  - **Team Builder**
  - **Team Runs**
- Settings: **Providers** (credentials + connection status)

## Primary Flow (3 steps, single screen)
1) **Create Team**
   - Team name (required)
   - Goal (optional, used as default context)
   - Default model/provider (optional)
   - Button: **Create**

2) **Add Agents**
   - Agent cards with:
     - Role (Planner / Executor / Critic / Researcher / Custom)
     - Provider (dropdown)
     - Model (dropdown filtered by provider)
     - Tools access (inherit team defaults by default)
     - Budget / rate cap (collapsed)
   - Quick actions:
     - **Add from template** (e.g. "3-agent standard team")
     - **Auto-assign roles**

3) **Run Task**
   - Task input (goal + constraints)
   - Optional advanced run settings (collapsed)
   - Button: **Run**

## Defaults and Progressive Disclosure
- Default to a "standard" 3-agent team (Planner, Executor, Critic).
- Collapse:
  - Tool policy
  - Budget/rate caps
  - Provider advanced params
  - System prompts
- Always show:
  - Role
  - Provider
  - Model
  - Status badges (Ready / Needs key / Invalid)

## Provider Credentials
- Located in **Settings → Providers**
- Status badges in Team Builder:
  - **Connected**
  - **Missing key** (one-click link to Providers)
  - **Invalid** (inline error + retry)

## Team Run UX
- Main view remains **Scene on top + Conversation below**.
- Team run view includes:
  - Role lanes
  - Clear agent labels (role + provider + model)
  - Tool output collapsed by default
  - "Latest outcome" banner

## Error States (Explicit + Actionable)
- Missing provider key → inline warning + "Fix in Settings"
- Unknown model → fallback suggestion + "Refresh models"
- Role conflict (two primaries) → show conflict and allow override
- Provider unavailable → allow reassign to another provider

## Accessibility & Clarity
- Use plain language labels ("Provider", "Model", "Role")
- Avoid internal IDs in the UI
- Prefer "Command" + "Output" over raw tool JSON
- Time-ordered conversation timeline

## Metrics to Validate
- Time to first run (target < 60s)
- Team creation abandonment rate
- % of users who use templates vs manual
- Error recovery rate (missing keys / invalid models)

## Non-Goals
- Multi-workspace org management
- Fine-grained policy language editing in the primary flow

