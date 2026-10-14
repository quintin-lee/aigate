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

const emit = defineEmits(['update:modelValue', 'change']);

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
        registerLuaCompletions();
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

function registerLuaCompletions() {
  if (!window.monaco || window.__aigateLuaCompletionsRegistered) return;
  window.__aigateLuaCompletionsRegistered = true;

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
        { label: 'aigate.log_info', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_info("${1:message}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '输出 INFO 级别运行日志' },
        { label: 'aigate.log_warn', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_warn("${1:message}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '输出 WARN 级别运行日志' },
        { label: 'aigate.log_error', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_error("${1:message}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '输出 ERROR 级别运行日志' },
        { label: 'aigate.sha256', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.sha256(${1:str})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '计算输入文本的 SHA-256 十六进制哈希摘要' },
        { label: 'aigate.json_decode', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.json_decode(${1:json_str})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '解析 JSON 字符串为 Lua Table 字典' },
        { label: 'aigate.json_encode', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.json_encode(${1:table})', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '序列化 Lua Table 为紧凑 JSON 字符串' },
        { label: 'aigate.abort', kind: window.monaco.languages.CompletionItemKind.Function, insertText: 'aigate.abort(${1:403}, "${2:forbidden}", "${3:Access denied}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '立即短路熔断并向客户端返回自定义 HTTP 错误状态' },
        { label: 'req:get_header', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:get_header("${1:header_name}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '获取指定请求头字段值 (忽略大小写)' },
        { label: 'req:set_header', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:set_header("${1:header_name}", "${2:value}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '添加或覆盖指定请求头' },
        { label: 'req:get_model', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:get_model()', documentation: '获取当前请求的大模型标识 (如 "gpt-4o")' },
        { label: 'req:set_model', kind: window.monaco.languages.CompletionItemKind.Method, insertText: 'req:set_model("${1:new_model}")', insertTextRules: window.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '动态改写路由大模型标识' },
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
      minimap: { enabled: false },
      scrollBeyondLastLine: false,
      fontSize: 13,
      lineHeight: 20,
      fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace',
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
      registerLuaCompletions();
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
