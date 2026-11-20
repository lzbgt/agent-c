"use strict";
const amount = document.querySelector("#amount");
const suggestions = [...document.querySelectorAll("[data-amount]")];
function reflectAmount() {
  for (const button of suggestions) {
    button.setAttribute("aria-pressed", String(Number(button.dataset.amount) === Number(amount.value)));
  }
}
for (const button of suggestions) {
  button.addEventListener("click", () => {
    amount.value = Number(button.dataset.amount).toFixed(2);
    reflectAmount();
  });
}
amount?.addEventListener("input", reflectAmount);
// A same-origin GET bridge keeps form-action restricted to this service. The
// server has already validated PayPal's exact approval origin, path and token.
const approval = document.querySelector("[data-paypal-redirect]");
if (approval) window.location.replace(approval.href);
const confirmation = document.querySelector("[data-return-confirm]");
if (confirmation) confirmation.requestSubmit();
for (const form of document.querySelectorAll("form")) {
  form.addEventListener("submit", () => {
    for (const button of form.querySelectorAll('button[type="submit"]')) {
      button.disabled = true;
      button.setAttribute("aria-busy", "true");
    }
  });
}
// Back/forward cache may restore a form after leaving PayPal.
window.addEventListener("pageshow", () => {
  for (const button of document.querySelectorAll('button[type="submit"]')) {
    button.disabled = false;
    button.removeAttribute("aria-busy");
  }
});
