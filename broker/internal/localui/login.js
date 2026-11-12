"use strict";
document.getElementById("login").addEventListener("submit", async (event) => {
  event.preventDefault();
  const input = document.getElementById("key");
  const button = document.getElementById("submit");
  const error = document.getElementById("error");
  button.disabled = true;
  error.textContent = "";
  try {
    const response = await fetch("/local/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ key: input.value.trim() }),
      credentials: "same-origin",
    });
    input.value = "";
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || "Unable to unlock the workspace");
    window.location.replace("/");
  } catch (e) {
    error.textContent = e instanceof Error ? e.message : "Unable to connect";
    button.disabled = false;
    input.focus();
  }
});
