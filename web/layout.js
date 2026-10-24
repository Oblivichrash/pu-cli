// SPDX-License-Identifier: GPL-3.0-only
// The panel's own half: which way it opens, and remembering that choice.

const shellEl = document.getElementById("shell");
const scrimEl = document.getElementById("scrim");

// Above the drawer breakpoint the panel is a column and remembers whether it is shown;
// below it the same class drives a drawer, so one choice covers both.
const NARROW = window.matchMedia("(max-width: 1024px)");

function panelVisible() {
  return !shellEl.classList.contains("sidebar-hidden");
}

function setPanelVisible(visible, remember) {
  shellEl.classList.toggle("sidebar-hidden", !visible);
  if (remember) {
    try { localStorage.setItem("pu.sidebar", visible ? "1" : "0"); } catch (_) {}
  }
  // The scrim only belongs to the drawer: on a wide screen the column is not overlay.
  const overlay = NARROW.matches && visible;
  if (scrimEl) scrimEl.hidden = !overlay;
}

document.getElementById("sidebar-toggle").addEventListener("click", () => {
  // On a wide screen the toggle is a preference and outlives the page; in the drawer it
  // is just opening and closing, which the size decides, so it is not stored.
  setPanelVisible(!panelVisible(), !NARROW.matches);
});

if (scrimEl) scrimEl.addEventListener("click", () => setPanelVisible(false, false));

// A default only applies where none was chosen: wide opens, narrow starts closed.
function applyPanelDefault() {
  let stored = null;
  try { stored = localStorage.getItem("pu.sidebar"); } catch (_) {}
  if (NARROW.matches) {
    setPanelVisible(false, false);
  } else if (stored === "0") {
    setPanelVisible(false, false);
  } else {
    setPanelVisible(true, false);
  }
}
applyPanelDefault();
NARROW.addEventListener("change", applyPanelDefault);
