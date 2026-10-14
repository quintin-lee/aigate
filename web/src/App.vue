<template>
  <n-config-provider :theme="darkTheme" :theme-overrides="darkThemeOverrides">
    <n-message-provider>
      <n-dialog-provider>
        <div class="flex h-screen w-screen overflow-hidden bg-slate-950 text-slate-100 font-sans">
          <!-- Sidebar for Desktop -->
          <aside class="hidden md:flex flex-col w-64 bg-slate-900/80 border-r border-slate-800 backdrop-blur-md z-20 flex-shrink-0">
            <!-- Brand header -->
            <div class="h-16 flex items-center px-6 border-b border-slate-800/80 space-x-3">
              <span class="text-2xl">⚡</span>
              <div class="flex flex-col">
                <span class="font-extrabold tracking-tight text-white text-base">aigate</span>
                <span class="text-[10px] text-slate-400 font-mono">高性能 AI 网关控制台</span>
              </div>
            </div>

            <!-- Nav list -->
            <nav class="flex-1 overflow-y-auto p-3 space-y-1 custom-scroll text-xs">
              <div
                v-for="item in navItems"
                :key="item.key"
                :id="'nav-' + item.key"
                :data-tab="item.key"
                @click="state.switchTab(item.key)"
                :class="[
                  'flex items-center space-x-2.5 px-3 py-2 rounded-lg cursor-pointer transition font-medium',
                  state.activeTab === item.key
                    ? 'bg-brand-600 text-white shadow-sm'
                    : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800/60'
                ]"
              >
                <span class="text-base">{{ item.icon }}</span>
                <span>{{ item.label }}</span>
              </div>
            </nav>

            <!-- Bottom status -->
            <div class="p-3 border-t border-slate-800 text-[11px] text-slate-500 font-mono text-center">
              aigate Core • C17 / CivetWeb / Lua
            </div>
          </aside>

          <!-- Mobile Sidebar Drawer -->
          <div
            v-if="state.sidebarOpen"
            class="fixed inset-0 z-50 md:hidden flex"
          >
            <div class="fixed inset-0 bg-slate-950/80 backdrop-blur-sm" @click="state.sidebarOpen = false"></div>
            <aside class="relative flex flex-col w-64 bg-slate-900 border-r border-slate-800 z-10">
              <div class="h-16 flex items-center justify-between px-6 border-b border-slate-800">
                <div class="flex items-center space-x-2">
                  <span class="text-xl">⚡</span>
                  <span class="font-bold text-white">aigate</span>
                </div>
                <button @click="state.sidebarOpen = false" class="text-slate-400 hover:text-white text-lg">✕</button>
              </div>
              <nav class="flex-1 overflow-y-auto p-3 space-y-1 text-xs">
                <div
                  v-for="item in navItems"
                  :key="item.key"
                  @click="state.switchTab(item.key)"
                  :class="[
                    'flex items-center space-x-2.5 px-3 py-2.5 rounded-lg cursor-pointer transition font-medium',
                    state.activeTab === item.key ? 'bg-brand-600 text-white' : 'text-slate-400 hover:text-white'
                  ]"
                >
                  <span>{{ item.icon }}</span>
                  <span>{{ item.label }}</span>
                </div>
              </nav>
            </aside>
          </div>

          <!-- Main Content Area -->
          <div class="flex-1 flex flex-col min-w-0 overflow-hidden">
            <!-- Top Navbar -->
            <header class="h-16 bg-slate-900/60 border-b border-slate-800/80 backdrop-blur-md flex items-center justify-between px-4 sm:px-6 z-10 flex-shrink-0">
              <div class="flex items-center space-x-3">
                <button
                  class="md:hidden p-2 rounded-lg text-slate-400 hover:text-white hover:bg-slate-800"
                  @click="state.sidebarOpen = true"
                >
                  ☰
                </button>
                <div class="flex items-center space-x-2">
                  <span class="text-base font-bold text-white">{{ currentTitle }}</span>
                  <span class="text-slate-600">/</span>
                  <span class="text-xs text-slate-400 font-mono">v1</span>
                </div>
              </div>

              <!-- Right Actions: Connection Badge & Token -->
              <div class="flex items-center space-x-3">
                <!-- Status Badge -->
                <div
                  :class="[
                    'hidden sm:flex items-center space-x-1.5 px-2.5 py-1 rounded-full text-xs font-medium border font-mono',
                    statusStyle.badge
                  ]"
                >
                  <span :class="['w-2 h-2 rounded-full', statusStyle.dot]"></span>
                  <span>{{ statusStyle.text }}</span>
                </div>

                <!-- Admin Token Button -->
                <button
                  @click="state.tokenModalVisible = true"
                  class="flex items-center space-x-1.5 px-3 py-1.5 rounded-lg border border-slate-700 bg-slate-800/80 hover:bg-slate-750 text-xs font-medium text-slate-200 transition"
                >
                  <span>🔑</span>
                  <span>{{ state.adminToken ? `${state.adminToken.slice(0, 8)}…` : '配置管理令牌' }}</span>
                </button>
              </div>
            </header>

            <!-- Scrollable Content View -->
            <main class="flex-1 overflow-y-auto p-4 sm:p-6 lg:p-8 custom-scroll">
              <div class="max-w-7xl mx-auto">
                <component :is="currentView" />
              </div>
            </main>
          </div>
        </div>

        <!-- Token Setup Modal -->
        <n-modal
          v-model:show="state.tokenModalVisible"
          preset="card"
          title="🔑 配置管理员令牌 (Admin Bearer Token)"
          class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
        >
          <div class="space-y-3">
            <p class="text-xs text-slate-400">
              请输入在网关配置文件或环境变量中指定的 <code class="text-cyan-400 font-mono">AIGATE_ADMIN_TOKEN</code>。该令牌将用于访问所有管理与指标接口。
            </p>
            <n-input
              v-model:value="tokenInput"
              type="password"
              show-password-on="click"
              placeholder="输入管理员令牌..."
            />
          </div>
          <template #footer>
            <div class="flex justify-end space-x-2">
              <n-button @click="state.tokenModalVisible = false">取消</n-button>
              <n-button type="primary" @click="saveToken">保存并认证</n-button>
            </div>
          </template>
        </n-modal>
      </n-dialog-provider>
    </n-message-provider>
  </n-config-provider>
</template>

<script setup>
import { ref, computed } from 'vue';
import {
  darkTheme,
  NConfigProvider,
  NMessageProvider,
  NDialogProvider,
  NModal,
  NInput,
  NButton,
} from 'naive-ui';
import { state } from '@/store/appStore';

// Views
import OverviewView from '@/views/OverviewView.vue';
import LiveView from '@/views/LiveView.vue';
import ProvidersView from '@/views/ProvidersView.vue';
import ModelsView from '@/views/ModelsView.vue';
import PromptsView from '@/views/PromptsView.vue';
import KeysView from '@/views/KeysView.vue';
import GuardrailsView from '@/views/GuardrailsView.vue';
import GroupsView from '@/views/GroupsView.vue';
import CostView from '@/views/CostView.vue';
import UsageView from '@/views/UsageView.vue';
import AuditView from '@/views/AuditView.vue';
import PlaygroundView from '@/views/PlaygroundView.vue';
import CacheView from '@/views/CacheView.vue';
import MetricsView from '@/views/MetricsView.vue';
import ShadowView from '@/views/ShadowView.vue';
import CompressorView from '@/views/CompressorView.vue';
import CacheOptimizerView from '@/views/CacheOptimizerView.vue';
import PluginsView from '@/views/PluginsView.vue';

const tokenInput = ref(state.adminToken);

const darkThemeOverrides = {
  common: {
    primaryColor: '#6366f1',
    primaryColorHover: '#818cf8',
    primaryColorPressed: '#4f46e5',
    primaryColorSuppl: '#6366f1',
    bodyColor: '#020617',
    cardColor: 'rgba(15, 23, 42, 0.75)',
    modalColor: '#0f172a',
    tableColor: 'transparent',
    headerColor: '#0f172a',
    borderColor: '#334155',
    textColorBase: '#f8fafc',
    textColor1: '#f1f5f9',
    textColor2: '#cbd5e1',
    textColor3: '#94a3b8',
    fontSize: '13px',
    borderRadius: '8px',
  },
  Card: {
    borderColor: 'rgba(51, 65, 85, 0.6)',
  },
  DataTable: {
    borderColor: 'rgba(51, 65, 85, 0.4)',
    thColor: 'rgba(30, 41, 59, 0.6)',
    tdColor: 'transparent',
    tdColorHover: 'rgba(30, 41, 59, 0.4)',
  },
};

const navItems = [
  { key: 'overview', icon: '📊', label: '总览看板' },
  { key: 'live', icon: '🛰️', label: '实时大屏' },
  { key: 'providers', icon: '🏢', label: '模型供应商' },
  { key: 'models', icon: '🤖', label: '上游模型' },
  { key: 'prompts', icon: '📝', label: 'Prompt 模板' },
  { key: 'keys', icon: '🔑', label: 'API 密钥' },
  { key: 'guardrails', icon: '🛡️', label: '安全风控' },
  { key: 'groups', icon: '👥', label: '部门分组' },
  { key: 'cost', icon: '💰', label: '成本归属' },
  { key: 'usage', icon: '📈', label: '用量统计' },
  { key: 'audit', icon: '📋', label: '审计日志' },
  { key: 'playground', icon: '⚡', label: '在线调试场' },
  { key: 'cache', icon: '💾', label: '响应缓存' },
  { key: 'metrics', icon: '🛠️', label: '监控指标' },
  { key: 'shadow', icon: '🪞', label: '流量镜像' },
  { key: 'compressor', icon: '🗜️', label: '提示词压缩' },
  { key: 'cache-optimizer', icon: '🎯', label: '缓存优化' },
  { key: 'plugins', icon: '🧩', label: '脚本插件' },
];

const viewMap = {
  overview: OverviewView,
  live: LiveView,
  providers: ProvidersView,
  models: ModelsView,
  prompts: PromptsView,
  keys: KeysView,
  guardrails: GuardrailsView,
  groups: GroupsView,
  cost: CostView,
  usage: UsageView,
  audit: AuditView,
  playground: PlaygroundView,
  cache: CacheView,
  metrics: MetricsView,
  shadow: ShadowView,
  compressor: CompressorView,
  'cache-optimizer': CacheOptimizerView,
  plugins: PluginsView,
};

const currentView = computed(() => {
  return viewMap[state.activeTab] || OverviewView;
});

const currentTitle = computed(() => {
  const item = navItems.find(i => i.key === state.activeTab);
  return item ? `${item.icon} ${item.label}` : '总览';
});

const statusStyle = computed(() => {
  if (state.connectionStatus === 'ok') {
    return {
      badge: 'bg-emerald-500/10 text-emerald-400 border-emerald-500/20',
      dot: 'bg-emerald-400 animate-pulse',
      text: '网关就绪',
    };
  }
  if (state.connectionStatus === 'failed') {
    return {
      badge: 'bg-rose-500/10 text-rose-400 border-rose-500/20',
      dot: 'bg-rose-400',
      text: '连接异常',
    };
  }
  return {
    badge: 'bg-slate-500/10 text-slate-400 border-slate-500/20',
    dot: 'bg-slate-400',
    text: '检测中',
  };
});

function saveToken() {
  state.setAdminToken(tokenInput.value.trim());
  state.tokenModalVisible = false;
  state.setConnectionStatus('checking');
}
</script>
