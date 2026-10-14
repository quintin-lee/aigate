<template>
  <div class="relative w-full h-full flex flex-col">
    <!-- Fallback textarea if offline -->
    <textarea
      v-if="offlineMode"
      v-model="internalValue"
      @input="onInput"
      class="w-full h-full bg-slate-950 font-mono text-xs text-slate-200 p-3 outline-none resize-none"
      placeholder="输入 Lua 5.4 脚本..."
    ></textarea>
    
    <!-- Monaco container -->
    <div
      v-show="!offlineMode"
      ref="editorContainer"
      class="w-full h-full flex-1"
    ></div>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount, watch } from 'vue';

const props = defineProps({
  modelValue: {
    type: String,
    default: '',
  },
  language: {
    type: String,
    default: 'lua',
  },
  readOnly: {
    type: Boolean,
    default: false,
  },
  diffMode: {
    type: Boolean,
    default: false,
  },
  originalValue: {
    type: String,
    default: '',
  },
});

const emit = defineEmits(['update:modelValue', 'change', 'save', 'run-test']);

const editorContainer = ref(null);
const offlineMode = ref(false);
const internalValue = ref(props.modelValue);

let editorInstance = null;
let diffEditorInstance = null;

function loadMonaco() {
  if (window.monaco) return Promise.resolve(window.monaco);

  return new Promise((resolve, reject) => {
    if (window.require && window.require.config) {
      window.require.config({
        paths: { vs: 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.45.0/min/vs' },
      });
      window.require(['vs/editor/editor.main'], () => {
        resolve(window.monaco);
      });
      return;
    }

    const script = document.createElement('script');
    script.src = 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.45.0/min/vs/loader.min.js';
    script.onload = () => {
      window.require.config({
        paths: { vs: 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.45.0/min/vs' },
      });
      window.require(['vs/editor/editor.main'], () => {
        registerLuaProviders();
        resolve(window.monaco);
      });
    };
    script.onerror = () => {
      offlineMode.value = true;
      reject(new Error('Monaco loader offline'));
    };
    document.head.appendChild(script);
  });
}

const HOVER_DOCS = {
  'abort_with': {
    title: 'aigate.abort_with(status, body)',
    doc: '**立即熔断阻断**\n\n以指定 HTTP 状态码和响应体中断请求流，后续插件及上游调用均被短路跳过。\n\n```lua\naigate.abort_with(403, \'{"error":{"code":"forbidden","message":"Access Denied"}}\')\n```',
  },
  'abort': {
    title: 'aigate.abort(status, reason, body)',
    doc: '**立即熔断阻断**\n\n短路中断流程并返回自定义 HTTP 错误状态。\n\n```lua\naigate.abort(401, "unauthorized", "Token expired")\n```',
  },
  'log_info': {
    title: 'aigate.log_info(msg)',
    doc: '**INFO 日志输出**\n\n向网关控制台及沙箱调试器输出青色 INFO 级别运行日志。\n\n```lua\naigate.log_info("处理请求: " .. req:get_model())\n```',
  },
  'log_warn': {
    title: 'aigate.log_warn(msg)',
    doc: '**WARN 告警日志**\n\n向网关控制台及沙箱调试器输出黄色 WARN 级别告警日志。',
  },
  'log_error': {
    title: 'aigate.log_error(msg)',
    doc: '**ERROR 错误日志**\n\n向网关控制台及沙箱调试器输出红色 ERROR 级别异常日志。',
  },
  'log_debug': {
    title: 'aigate.log_debug(msg)',
    doc: '**DEBUG 调试日志**\n\n向网关控制台及沙箱调试器输出暗灰 DEBUG 级别详细日志。',
  },
  'sha256': {
    title: 'aigate.sha256(str)',
    doc: '**SHA-256 哈希计算**\n\n计算输入文本的 64 字符十六进制 SHA-256 哈希值。\n\n```lua\nlocal hash = aigate.sha256("api-key-here")\n```',
  },
  'json_decode': {
    title: 'aigate.json_decode(str)',
    doc: '**JSON 反序列化**\n\n将 JSON 字符串解析为 Lua Table 字典对象。解析失败返回 `nil`。',
  },
  'json_encode': {
    title: 'aigate.json_encode(tbl)',
    doc: '**JSON 序列化**\n\n将 Lua Table 对象序列化为紧凑 JSON 字符串。',
  },
  'get_header': {
    title: 'req:get_header(name) / resp:get_header(name)',
    doc: '**获取 HTTP 请求头 / 响应头**\n\n获取指定 Header 字段值（不区分大小写）。若字段不存在返回 `nil`。\n\n```lua\nlocal auth = req:get_header("authorization")\n```',
  },
  'set_header': {
    title: 'req:set_header(name, value) / resp:set_header(name, value)',
    doc: '**注入/覆盖 HTTP 请求头或响应头**\n\n添加或更新指定 Header 字段。\n\n```lua\nreq:set_header("x-gateway-trace", "tr-123456")\n```',
  },
  'get_model': {
    title: 'req:get_model()',
    doc: '**获取请求大模型**\n\n获取当前请求的目标大模型标识（如 `"gpt-4o"`、`"claude-3-7-sonnet"`）。',
  },
  'set_model': {
    title: 'req:set_model(name)',
    doc: '**动态路由改写大模型**\n\n无感重写路由的目标大模型（如重路由至 `"deepseek-chat"`）。',
  },
  'get_client_ip': {
    title: 'req:get_client_ip()',
    doc: '**获取客户端 IP**\n\n获取请求来源的真实 IP 地址（如 `"192.168.1.100"`），可用于频控与黑名单。',
  },
  'get_body': {
    title: 'req:get_body() / resp:get_body()',
    doc: '**获取请求体 / 响应体**\n\n获取原始 HTTP 文本载荷。出站阶段可获取上游返回的完整补全内容。',
  },
  'set_body': {
    title: 'req:set_body(new_body) / resp:set_body(new_body)',
    doc: '**改写替换请求体 / 响应体**\n\n替换 HTTP Body 内容（可用于敏感信息掩码脱敏、内容审查、篡改）。',
  },
  'get_json': {
    title: 'req:get_json()',
    doc: '**快捷获取解析后的 JSON 对象**\n\n直接将请求体解析为 Lua Table 对象。',
  },
  'get_status': {
    title: 'resp:get_status()',
    doc: '**获取上游 HTTP 状态码**\n\n返回出站响应的状态码（如 `200`、`500`）。',
  },
  'set_status': {
    title: 'resp:set_status(code)',
    doc: '**修改客户端 HTTP 状态码**\n\n出站阶段重写响应状态码（如改为 `429` 或 `503`）。',
  },
};

function registerLuaProviders() {
  if (!window.monaco || window.__aigateLuaProvidersRegistered) return;
  window.__aigateLuaProvidersRegistered = true;

  // 1. Completion & Snippets Provider
  window.monaco.languages.registerCompletionItemProvider('lua', {
    provideCompletionItems: (model, position) => {
      const word = model.getWordUntilPosition(position);
      const range = {
        startLineNumber: position.lineNumber,
        endLineNumber: position.lineNumber,
        startColumn: word.startColumn,
        endColumn: word.endColumn,
      };

      const suggestions = [
        // Snippets
        {
          label: 'req-header-check',
          kind: window.monaco.languages.CompletionItemKind.Snippet,
          insertText: [
            'local ${1:auth_val} = req:get_header("${2:authorization}")',
            'if not ${1:auth_val} then',
            '    aigate.abort_with(401, \'{"error":{"code":"unauthorized","message":"Missing required header"}}\')',
            '    return',
            'end'
          ].join('\n'),
          insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          documentation: '校验指定 Header，若缺失则立即以 401 熔断阻断',
        },
        {
          label: 'req-model-rewrite',
          kind: window.monaco.languages.CompletionItemKind.Snippet,
          insertText: [
            'local model = req:get_model()',
            'if model == "${1:gpt-4o}" then',
            '    req:set_model("${2:deepseek-chat}")',
            '    aigate.log_info("Model rewritten to ${2:deepseek-chat}")',
            'end'
          ].join('\n'),
          insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          documentation: '条件判断并动态改写请求大模型名称',
        },
        {
          label: 'json-modify-body',
          kind: window.monaco.languages.CompletionItemKind.Snippet,
          insertText: [
            'local body = req:get_body()',
            'if body and body ~= "" then',
            '    local new_body = string.gsub(body, "${1:search}", "${2:replace}")',
            '    req:set_body(new_body)',
            'end'
          ].join('\n'),
          insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          documentation: '读取并文本替换改写 HTTP Body 载荷',
        },
        {
          label: 'aigate-abort',
          kind: window.monaco.languages.CompletionItemKind.Snippet,
          insertText: 'aigate.abort_with(${1:403}, \'{"error":{"code":"forbidden","message":"${2:Access Denied}"}}\')',
          insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          documentation: '立即以指定 HTTP 状态码与 JSON 载荷阻断请求',
        },
        {
          label: 'log-suite',
          kind: window.monaco.languages.CompletionItemKind.Snippet,
          insertText: [
            'aigate.log_info("${1:info message}")',
            'aigate.log_warn("${2:warn message}")'
          ].join('\n'),
          insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          documentation: '插入常用网关日志打印语句',
        },
        {
          label: 'req-client-ip',
          kind: window.monaco.languages.CompletionItemKind.Snippet,
          insertText: [
            'local client_ip = req:get_client_ip()',
            'aigate.log_info("Request from IP: " .. (client_ip or "unknown"))'
          ].join('\n'),
          insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          documentation: '获取客户端来源 IP 并打印日志',
        },

        // Global functions & Methods
        { label: 'aigate.abort_with', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.abort_with(${1:403}, \'{"error":"${2:forbidden}"}\')', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '立即以 HTTP 状态码与内容阻断请求' },
        { label: 'aigate.abort', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.abort(${1:403}, "${2:forbidden}", "${3:Access denied}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '立即短路熔断' },
        { label: 'aigate.log_info', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_info("${1:message}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '输出 INFO 级别运行日志' },
        { label: 'aigate.log_warn', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_warn("${1:message}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '输出 WARN 级别告警日志' },
        { label: 'aigate.log_error', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_error("${1:message}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '输出 ERROR 级别错误日志' },
        { label: 'aigate.log_debug', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_debug("${1:message}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '输出 DEBUG 级别调试日志' },
        { label: 'aigate.sha256', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.sha256(${1:str})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '计算输入文本的 SHA-256 十六进制哈希摘要' },
        { label: 'aigate.json_decode', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.json_decode(${1:json_str})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '解析 JSON 字符串为 Lua Table' },
        { label: 'aigate.json_encode', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.json_encode(${1:table})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '序列化 Lua Table 为 JSON 字符串' },

        { label: 'req:get_header', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:get_header("${1:header_name}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '获取指定请求头字段值 (忽略大小写)' },
        { label: 'req:set_header', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:set_header("${1:header_name}", "${2:value}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '添加或覆盖指定请求头' },
        { label: 'req:get_model', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:get_model()', documentation: '获取当前请求的大模型标识 (如 "gpt-4o")' },
        { label: 'req:set_model', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:set_model("${1:new_model}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '动态改写路由大模型标识' },
        { label: 'req:get_client_ip', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:get_client_ip()', documentation: '获取客户端真实来源 IP' },
        { label: 'req:get_body', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:get_body()', documentation: '获取原始请求体 Body 字符串' },
        { label: 'req:set_body', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:set_body(${1:new_body})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '改写替换请求体 Body 内容' },
        { label: 'req:get_json', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:get_json()', documentation: '解析请求体 JSON 为 Lua Table 对象' },

        { label: 'resp:get_header', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'resp:get_header("${1:header_name}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '获取指定响应头字段值' },
        { label: 'resp:set_header', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'resp:set_header("${1:header_name}", "${2:value}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '添加或覆盖响应头' },
        { label: 'resp:get_status', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'resp:get_status()', documentation: '获取上游响应 HTTP 状态码' },
        { label: 'resp:set_status', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'resp:set_status(${1:status_code})', documentation: '修改客户端接收的 HTTP 状态码' },
        { label: 'resp:get_body', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'resp:get_body()', documentation: '获取上游响应体内容' },
        { label: 'resp:set_body', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'resp:set_body(${1:new_body})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '改写替换最终响应体内容' },
      ];

      return { suggestions: suggestions.map(s => ({ ...s, range })) };
    },
  });

  // 2. Hover Documentation Provider
  window.monaco.languages.registerHoverProvider('lua', {
    provideHover: (model, position) => {
      const word = model.getWordAtPosition(position);
      if (!word) return null;

      const target = HOVER_DOCS[word.word];
      if (!target) return null;

      return {
        range: new window.monaco.Range(
          position.lineNumber,
          word.startColumn,
          position.lineNumber,
          word.endColumn
        ),
        contents: [
          { value: `### \`${target.title}\`` },
          { value: target.doc },
        ],
      };
    },
  });
}

function initEditor() {
  if (!editorContainer.value) return;

  if (props.diffMode) {
    diffEditorInstance = window.monaco.editor.createDiffEditor(editorContainer.value, {
      theme: 'vs-dark',
      readOnly: true,
      automaticLayout: true,
      minimap: { enabled: false },
      scrollBeyondLastLine: false,
      renderSideBySide: true,
    });

    diffEditorInstance.setModel({
      original: window.monaco.editor.createModel(props.originalValue, props.language),
      modified: window.monaco.editor.createModel(props.modelValue, props.language),
    });
  } else {
    editorInstance = window.monaco.editor.create(editorContainer.value, {
      value: props.modelValue,
      language: props.language,
      theme: 'vs-dark',
      readOnly: props.readOnly,
      automaticLayout: true,
      bracketPairColorization: { enabled: true },
      minimap: { enabled: true },
      scrollBeyondLastLine: false,
      fontSize: 13,
      lineHeight: 20,
      fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace',
    });

    // Keyboard Shortcuts
    editorInstance.addCommand(window.monaco.KeyMod.CtrlCmd | window.monaco.KeyCode.KeyS, () => {
      emit('save');
    });
    editorInstance.addCommand(window.monaco.KeyMod.CtrlCmd | window.monaco.KeyCode.Enter, () => {
      emit('run-test');
    });

    editorInstance.onDidChangeModelContent(() => {
      const val = editorInstance.getValue();
      internalValue.value = val;
      emit('update:modelValue', val);
      emit('change', val);
    });
  }
}

function setMarkers(markers) {
  if (editorInstance && window.monaco) {
    const model = editorInstance.getModel();
    if (model) {
      window.monaco.editor.setModelMarkers(model, 'owner', markers);
    }
  }
}

function onInput(e) {
  emit('update:modelValue', e.target.value);
  emit('change', e.target.value);
}

watch(
  () => props.modelValue,
  (newVal) => {
    internalValue.value = newVal;
    if (editorInstance && editorInstance.getValue() !== newVal) {
      editorInstance.setValue(newVal || '');
    }
    if (diffEditorInstance) {
      const model = diffEditorInstance.getModel();
      if (model && model.modified) {
        model.modified.setValue(newVal || '');
      }
    }
  }
);

watch(
  () => props.originalValue,
  (newVal) => {
    if (diffEditorInstance) {
      const model = diffEditorInstance.getModel();
      if (model && model.original) {
        model.original.setValue(newVal || '');
      }
    }
  }
);

onMounted(() => {
  loadMonaco()
    .then(() => {
      registerLuaProviders();
      initEditor();
    })
    .catch(() => {
      offlineMode.value = true;
    });
});

onBeforeUnmount(() => {
  if (editorInstance) editorInstance.dispose();
  if (diffEditorInstance) diffEditorInstance.dispose();
});

defineExpose({
  setMarkers,
});
</script>
