import type { BrokerTeamRunGoalContract } from "../api";
import type { Attachment } from "../components/PromptBar";

export function buildGoalContractFromPrompt(promptRaw: string): BrokerTeamRunGoalContract | null {
  const lines = String(promptRaw || "")
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.length > 0);
  if (lines.length === 0) return null;
  const [goal, ...rest] = lines;
  const contract: BrokerTeamRunGoalContract = { goal };
  if (rest.length > 0) contract.success_criteria = rest;
  return contract;
}

export function collectTeamUploadFiles(attachments: Attachment[]) {
  return (attachments || []).filter((attachment) => {
    return typeof attachment?.data_base64 === "string" && attachment.data_base64.length > 0;
  });
}
