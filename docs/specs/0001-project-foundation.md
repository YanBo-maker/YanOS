# 0001：项目基础

## INTENTION

在 CPU 开发开始前建立项目说明、开发流程和 Git 约定，为后续实现与审查提供依据。

本次范围为文档和仓库配置。CPU 实现与 C 工具链配置另行开展。

## SPEC

- 默认分支为 `main`。
- 技术方向采用 C17 功能模拟器，本机运行优先。
- 开发遵循 INTENTION → SPEC → IMPLE → VERIFY。
- 后续开发使用短期任务分支和 PR，提交按逻辑变化划分。
- 文本使用统一换行规则，生成文件与运行数据由 Git 忽略。
- PR 模板包含问题、规格、验证和限制。

## IMPLE PLAN

添加 `.gitignore`、`.gitattributes`、`README.md`、`CONTRIBUTING.md`、`AGENTS.md`、`docs/STATUS.md`、本规格及 PR 模板。

初始化直接提交至 `main`，分为仓库配置、项目与协作准则、PR 模板三个逻辑单元。

## VERIFY

初始化已检查文档本地链接、忽略规则、二进制文件换行属性和提交差异。该阶段尚无可执行代码。
