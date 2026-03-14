import React from "react";

import ConversationEventList from "./conversation/ConversationEventList";
import useConversationViewState from "./conversation/useConversationViewState";
import type { ConversationViewProps } from "./conversation/conversationViewTypes";

export default function ConversationView(props: ConversationViewProps) {
  const state = useConversationViewState(props);
  return <ConversationEventList {...props} {...state} />;
}
