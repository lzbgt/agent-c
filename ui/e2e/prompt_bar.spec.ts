import { expect, test } from "@playwright/test";

test("prompt bar stages uploads, exposes drawer details, and collapses with preview", async ({ page }) => {
  let uploadBody: any = null;
  const corsHeaders = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,POST,OPTIONS",
    "access-control-allow-headers": "authorization,content-type",
  };

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7779");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7779/api/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();

    if (method === "OPTIONS") {
      await route.fulfill({ status: 204, headers: corsHeaders, body: "" });
      return;
    }

    if (method === "POST" && path === "/api/v1/session/upload") {
      uploadBody = request.postDataJSON();
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({
          ok: true,
          session_id: "default",
          files: [{ path: "uploads/note.md", name: "note.md", mime: "text/plain", kind: "text/plain", bytes: 12 }],
        }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/config") {
      await route.fulfill({ status: 200, contentType: "application/json", headers: corsHeaders, body: JSON.stringify({ ok: true }) });
      return;
    }

    await route.fallback();
  });

  await page.goto("/");

  await page.getByTestId("prompt").fill("summarize the staged file");
  await page.getByTestId("promptbar-toggle-details").click();
  await expect(page.getByTestId("promptbar-details")).toBeVisible();
  await expect(page.getByTestId("promptbar-details")).toContainText("session=default");

  await expect(page.getByTestId("promptbar-attach-button")).toBeVisible();
  await page.getByTestId("promptbar-file-input").setInputFiles({
    name: "note.md",
    mimeType: "text/plain",
    buffer: Buffer.from("hello world\n"),
  });

  await expect.poll(() => uploadBody).not.toBeNull();
  expect(uploadBody).toEqual({
    session_id: "default",
    files: [{ name: "note.md", mime: "text/plain", data_base64: Buffer.from("hello world\n").toString("base64") }],
  });

  await expect(page.getByTestId("promptbar-attachment-count")).toContainText("Staged: 1");
  await expect(page.getByTestId("promptbar-attachments-section")).toContainText("note.md");
  await expect(page.getByTestId("promptbar-details")).toContainText("uploaded 1 file(s)");

  await page.getByRole("button", { name: "Close" }).click();
  await page.getByTestId("promptbar-toggle-collapse").click();
  await expect(page.getByTestId("prompt")).toHaveCount(0);
  await expect(page.getByTestId("promptbar-toggle-collapse")).toContainText("summarize the staged file");
  await expect(page.getByTestId("promptbar-toggle-collapse")).toContainText("(1 attachment)");

  await page.getByTestId("promptbar-toggle-details").click();
  await expect(page.getByTestId("promptbar-details")).toBeVisible();
  await page.getByRole("button", { name: /Clear \(1\)/ }).click();
  await expect(page.getByTestId("promptbar-attachments-section")).toContainText("No attachments staged for the next run.");
});
