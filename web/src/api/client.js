import { state } from "@/store/appStore";

export async function apiCall(endpoint, options = {}) {
  const headers = options.headers || {};
  if (state.adminToken && !headers["Authorization"]) {
    headers["Authorization"] = `Bearer ${state.adminToken}`;
  }
  if (options.body && typeof options.body === "object" && !(options.body instanceof FormData)) {
    headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(options.body);
  }
  options.headers = headers;

  try {
    const resp = await fetch(endpoint, options);
    if (resp.status === 401) {
      state.setConnectionStatus("failed");
      state.tokenModalVisible = true;
      return null;
    }
    if (resp.ok) {
      state.setConnectionStatus("ok");
    }
    return resp;
  } catch (err) {
    state.setConnectionStatus("failed");
    console.error("API error:", err);
    return null;
  }
}

export const api = {
  async get(url, options = {}) {
    return apiCall(url, { ...options, method: "GET" });
  },
  async post(url, body, options = {}) {
    return apiCall(url, { ...options, method: "POST", body });
  },
  async put(url, body, options = {}) {
    return apiCall(url, { ...options, method: "PUT", body });
  },
  async delete(url, options = {}) {
    return apiCall(url, { ...options, method: "DELETE" });
  },
  async getJson(url, options = {}) {
    const resp = await this.get(url, options);
    if (!resp || !resp.ok) return null;
    try {
      return await resp.json();
    } catch {
      return null;
    }
  },
  async postJson(url, body, options = {}) {
    const resp = await this.post(url, body, options);
    if (!resp || !resp.ok) return null;
    try {
      return await resp.json();
    } catch {
      return null;
    }
  },
};
