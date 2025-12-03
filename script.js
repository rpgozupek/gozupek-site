// Mobil menü
const navToggle = document.getElementById("navToggle");
const mainNav = document.getElementById("mainNav");

if (navToggle && mainNav) {
  navToggle.addEventListener("click", () => {
    mainNav.classList.toggle("open");
  });
}

// Yıl otomatik
const yearSpan = document.getElementById("year");
if (yearSpan) {
  yearSpan.textContent = new Date().getFullYear();
}
