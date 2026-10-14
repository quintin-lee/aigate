import { reactive } from "vue";

export const state = reactive({
  adminToken: localStorage.getItem("aigate_admin_token") || "",
  connectionStatus: "checking", // "checking" | "ok" | "failed"
  activeTab: "overview",
  tokenModalVisible: false,
  sidebarOpen: false,

  // Shared entity caches
  providers: [],
  models: [],
  keys: [],
  groups: [],
  guardrails: [],
  plugins: [],

  // Global methods
  setAdminToken(token) {
    this.adminToken = token;
    if (token) {
      localStorage.setItem("aigate_admin_token", token);
    } else {
      localStorage.removeItem("aigate_admin_token");
    }
  },
  setConnectionStatus(status) {
    this.connectionStatus = status;
  },
  switchTab(tab) {
    this.activeTab = tab;
    this.sidebarOpen = false;
  },
});
