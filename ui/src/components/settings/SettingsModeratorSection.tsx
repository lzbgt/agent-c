import React from "react";
import type { ModeratorEvent } from "../../api";
import type { ConnectionSettings } from "../../hooks/uiSettingsTypes";
import { SectionHeader } from "./SettingsControls";
import SettingsModeratorEventsSection from "./SettingsModeratorEventsSection";
import SettingsModeratorPublishSection from "./SettingsModeratorPublishSection";

type SettingsModeratorSectionProps = {
  connection: ConnectionSettings;
  sessionId: string;
  moderatorDirective: string;
  setModeratorDirective: React.Dispatch<React.SetStateAction<string>>;
  moderatorDirectiveScope: string;
  setModeratorDirectiveScope: React.Dispatch<React.SetStateAction<string>>;
  moderatorDirectiveAssignees: string;
  setModeratorDirectiveAssignees: React.Dispatch<React.SetStateAction<string>>;
  moderatorDirectivePick: string;
  setModeratorDirectivePick: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskTitle: string;
  setModeratorTaskTitle: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskDetail: string;
  setModeratorTaskDetail: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskAssignees: string;
  setModeratorTaskAssignees: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskPick: string;
  setModeratorTaskPick: React.Dispatch<React.SetStateAction<string>>;
  moderatorAppendToSession: boolean;
  setModeratorAppendToSession: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorBusy: boolean;
  moderatorError: string | null;
  moderatorSuccess: string | null;
  moderatorEventsAuto: boolean;
  setModeratorEventsAuto: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsMaxBytes: string;
  setModeratorEventsMaxBytes: React.Dispatch<React.SetStateAction<string>>;
  moderatorEventsIncludeDirectives: boolean;
  setModeratorEventsIncludeDirectives: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsIncludeTasks: boolean;
  setModeratorEventsIncludeTasks: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsFilter: string;
  setModeratorEventsFilter: React.Dispatch<React.SetStateAction<string>>;
  moderatorEventsExpanded: Record<string, boolean>;
  setModeratorEventsExpanded: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  brokerAgentOptions: Array<{ id: string; label: string; connected: boolean }>;
  brokerAgentsBusy: boolean;
  listBrokerAgents: () => Promise<void>;
  moderatorRolePresets: string[];
  addDirectiveAssignee: (value: string) => void;
  addTaskAssignee: (value: string) => void;
  applyRuntimeMemberTaskTemplate: () => void;
  publishModeratorDirective: () => Promise<void>;
  publishModeratorTask: () => Promise<void>;
  moderatorEventsEnabled: boolean;
  moderatorEventsRefetch: () => void;
  moderatorEventsFetching: boolean;
  moderatorEventsError: string | null;
  moderatorEventsList: ModeratorEvent[];
  moderatorEventsFiltered: ModeratorEvent[];
  moderatorPinnedEvents: Record<string, ModeratorEvent>;
  moderatorPinnedEntries: Array<[string, ModeratorEvent]>;
  updateModeratorPinnedEvents: (
    updater: Record<string, ModeratorEvent> | ((prev: Record<string, ModeratorEvent>) => Record<string, ModeratorEvent>),
  ) => void;
  pinImportRef: React.RefObject<HTMLInputElement>;
  showPinNotice: (msg: string, ok: boolean) => void;
  handleCopy: (label: string, text: string) => Promise<void>;
  pinnedCompareOptions: Array<{ key: string; label: string }>;
  pinnedCompareA: string;
  setPinnedCompareA: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareB: string;
  setPinnedCompareB: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareDiffOnly: boolean;
  setPinnedCompareDiffOnly: React.Dispatch<React.SetStateAction<boolean>>;
  copyNotice: string | null;
  pinNotice: string | null;
  pinError: string | null;
  moderatorDirectivesEnabled: boolean;
  moderatorTasksEnabled: boolean;
};

export default function SettingsModeratorSection(props: SettingsModeratorSectionProps) {
  const {
    connection,
    sessionId,
  } = props;

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <SectionHeader title="Moderator" />
      <div className="mt-2">
        <SettingsModeratorPublishSection {...props} />
        <SettingsModeratorEventsSection {...props} />
        <div className="text-[11px] text-white/50">
          Moderator directives/tasks are stored as client events. Assignees and scope are advisory hints; empty assignees
          broadcast to all listening agents.
        </div>
      </div>
    </div>
  );
}
