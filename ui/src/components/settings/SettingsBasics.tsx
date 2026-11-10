import type { SettingsDrawerProps } from "./settingsDrawerTypes";

export default function SettingsBasics({ connection, run, session }: Pick<SettingsDrawerProps, "connection" | "run" | "session">) {
  return <section className="settings-basics" aria-label="Conversation settings">
    <label>Model<input aria-label="Model" value={run.model} onChange={(event) => run.setModel(event.target.value)} placeholder="Use the server default" /></label>
    <label>Agent capabilities<select aria-label="Agent capabilities" value={run.tools} onChange={(event) => run.setTools(event.target.value as "host" | "basic" | "none")}>
      <option value="host">Work with files and tools</option>
      <option value="basic">Use basic tools</option>
      <option value="none">Conversation only</option>
    </select></label>
    <label>Conversation<input aria-label="Conversation" data-testid="session-id-input" list="saved-conversations" value={session.id} onChange={(event) => session.setId(event.target.value)} /></label>
    <datalist id="saved-conversations">{session.sessions.map((id) => <option value={id} key={id} />)}</datalist>
    <button className="workspace-button" type="button" disabled={session.newSessionPending} onClick={session.newSession}>New conversation</button>
    {connection.profiles.length > 1 ? <label>Environment<select aria-label="Environment" value={connection.activeProfileId} onChange={(event) => connection.setActiveProfileId(event.target.value)}>
      {connection.profiles.map((profile) => <option key={profile.id} value={profile.id}>{profile.name}</option>)}
    </select></label> : null}
  </section>;
}
