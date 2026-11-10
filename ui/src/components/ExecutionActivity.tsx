import React from "react";

import ExecutionEventList from "./conversation/ExecutionEventList";
import useConversationViewState from "./conversation/useConversationViewState";
import type { ConversationViewProps } from "./conversation/conversationViewTypes";

export default function ExecutionActivity(props: ConversationViewProps) {
  const state = useConversationViewState(props);
  return <ExecutionEventList {...props} {...state} />;
}
