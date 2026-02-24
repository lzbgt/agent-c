export type BrokerEventRow = {
  type: string;
  ts_unix_ms?: number;
  event_id?: string;
  trace_id?: string;
  payload?: Record<string, any>;
};
