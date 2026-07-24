# DEEPSEEK 重构指令：代码库简化与健康清理（分步执行版）

## 背景

Phase 1–7 已合并至 `develop` 分支，当前代码库功能完整，但存在以下可优化点：

- 重复定义（`agent_config.hpp` 与 `agent_core.hpp` 中的配置结构体）
- 死代码（`src/config/error_codes.cpp` 已从构建中排除）
- 错误处理不统一（`HttpError`/`StoreError` 与 `std::runtime_error` 混用）
- 遗留组件（`GlobalContext` 与 `core::Context` 功能重叠）
- 目录结构冗余（`orchestrator/` 单文件目录，`renderer.cpp` 放错位置）
- CMake 库划分过细（7 个静态库，增加维护成本）

**目标**：在不破坏现有功能的前提下，分小步完成代码清理，为后续 MCP/Skills 扩展铺路。

---

## 执行原则

1. **每步独立**：每完成一步即可编译、测试，确保不破坏主分支。
2. **渐进提交**：每步单独 `commit`，便于回滚与审查。
3. **向后兼容**：所有改动不破坏现有 `agents.json` 配置与用户数据（`~/.pu/`）。
4. **测试先行**：每步完成后运行 `ctest` 确保全部通过。

---

## 步骤清单（共 8 步）

| 步骤 | 名称 | 预计耗时 |
|------|------|----------|
| **1** | 建立健康基线（扫描报告） | 15 min |
| **2** | 删除死代码与重复定义 | 20 min |
| **3** | 统一错误处理体系 | 30 min |
| **4** | GlobalContext → core::Context 双写迁移 | 45 min |
| **5** | 目录重组（移动文件） | 30 min |
| **6** | CMake 库结构简化 | 30 min |
| **7** | 更新文档（README / CONTRIBUTING） | 20 min |
| **8** | 最终验证与覆盖率检查 | 30 min |

---

## 详细执行指令

### 步骤 1：建立健康基线（扫描报告）

**目的**：记录当前代码状态，作为后续改进的参照。

**操作**：

```bash
# 在项目根目录执行
mkdir -p docs/health

# 文件统计
find src include -type f \( -name "*.cpp" -o -name "*.hpp" \) | wc -l > docs/health/file_count.txt

# 代码行数（需安装 cloc）
cloc --exclude-dir=tests,build src include --json > docs/health/cloc_report.json

# 扫描技术债标记
grep -rn "TODO\|FIXME\|HACK" src include --include="*.cpp" --include="*.hpp" > docs/health/todos.txt 2>/dev/null

# 扫描 GlobalContext 使用
grep -rn "GlobalContext" src include --include="*.cpp" --include="*.hpp" > docs/health/global_context_usage.txt 2>/dev/null

# 扫描裸露 new/delete
grep -rn "new \|delete " src --include="*.cpp" | grep -v "delete\s*\[" | grep -v "unique_ptr\|shared_ptr" > docs/health/raw_allocations.txt 2>/dev/null

# 生成汇总报告
cat > docs/health/health_summary.md << 'EOF'
# 健康检查基线
- 日期: $(date)
- 总文件数: $(cat docs/health/file_count.txt)
- 技术债标记数: $(cat docs/health/todos.txt | wc -l)
- GlobalContext 引用数: $(cat docs/health/global_context_usage.txt | wc -l)
EOF
```

**验收**：`docs/health/` 目录下生成 5 个报告文件，内容非空。

---

### 步骤 2：删除死代码与重复定义

**目的**：移除已废弃或重复的文件，减少维护负担。

**操作**：

```bash
# 2.1 删除重复的 agent_config.hpp（内容已合并到 agent_core.hpp）
rm include/pu/agent_config.hpp

# 确认没有文件引用它
grep -rn "agent_config.hpp" src include --include="*.cpp" --include="*.hpp" | grep -v "rm"
# 如果无输出，则安全删除

# 2.2 删除已从构建中排除的 error_codes.cpp 及整个 config 目录
rm -rf src/config/

# 2.3 将设计草案移至 docs/design/（保留供参考）
mkdir -p docs/design
mv DEEPSEEK.md MCP.md SKILLS.md docs/design/ 2>/dev/null || true

# 2.4 从 CMakeLists.txt 中移除对 src/config/error_codes.cpp 的引用（确认无该行）
# 检查 CMakeLists.txt 是否还有 error_codes 相关行，若有则删除
```

**验收**：
- `include/pu/agent_config.hpp` 不存在
- `src/config/` 目录不存在
- 根目录下 `DEEPSEEK.md`、`MCP.md`、`SKILLS.md` 已移走
- 编译通过：`cmake --build build` 无错误

**提交信息**：
```
cleanup: remove dead code and duplicate headers

- Delete include/pu/agent_config.hpp (content duplicated in agent_core.hpp)
- Remove src/config/error_codes.cpp and src/config/ directory
- Move design drafts to docs/design/
```

---

### 步骤 3：统一错误处理体系

**目的**：建立统一的异常基类，便于捕获和日志记录。

**操作**：

修改 `include/pu/error.hpp`：

```cpp
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>

namespace pu {

// 新增：项目统一异常基类
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 令 HttpError 继承自 pu::Error 而非直接继承 std::runtime_error
class HttpError : public Error {
public:
    explicit HttpError(const std::string& msg) : Error(msg), detail_(msg) {}
    explicit HttpError(const std::string& msg, const std::string& detail)
        : Error(msg), detail_(detail) {}
    const std::string& detail() const { return detail_; }

private:
    std::string detail_;
};

// 令 StoreError 继承自 pu::Error
class StoreError : public Error {
public:
    explicit StoreError(const std::string& msg) : Error(msg) {}
};

}  // namespace pu
```

**全局替换（可选）**：查找所有 `throw std::runtime_error`，可逐步替换为 `throw pu::Error`，但本次步骤**不强求全部替换**，仅确保新代码使用 `pu::Error`。

**验收**：
- `pu::Error` 可在 `catch (const pu::Error&)` 中捕获所有项目异常
- 原有 `HttpError`/`StoreError` 捕获逻辑仍然有效（因它们仍继承自 `std::runtime_error`）
- 编译通过，测试通过

**提交信息**：
```
feat(error): add pu::Error base class for unified exception handling

- Define pu::Error inheriting from std::runtime_error
- Make HttpError and StoreError inherit from pu::Error
- Allows catching all project exceptions via pu::Error&
```

---

### 步骤 4：GlobalContext → core::Context 双写迁移

**目的**：将笔记和摘要从 `GlobalContext` 逐步迁移到 `core::Context`，采用双写模式保证兼容。

**操作**：

修改 `src/app/session.cpp`：

```cpp
// 在 AddNote 中增加 core::Context 写入
void SessionManager::AddNote(const std::string& agent_name, const std::string& text,
                             GlobalContext& global_ctx,
                             std::shared_ptr<core::Context> root_context) {
    std::string timestamped_note = "[" + CurrentTimestamp() + "] " + text;

    // --- 原有 GlobalContext 写入（保留） ---
    auto notes_opt = global_ctx.Read("memory/notes/" + agent_name);
    nlohmann::json notes_array = nlohmann::json::array();
    if (notes_opt && notes_opt->is_array()) {
        notes_array = *notes_opt;
    }
    notes_array.push_back(timestamped_note);
    global_ctx.Write("memory/notes/" + agent_name, notes_array);

    // --- 新增：写入 core::Context ---
    if (root_context) {
        std::string var_key = "notes/" + agent_name;
        auto existing = root_context->GetVar(var_key);
        nlohmann::json new_notes = existing.has_value() ? *existing : nlohmann::json::array();
        if (new_notes.is_array()) {
            new_notes.push_back(timestamped_note);
            root_context->SetVar(var_key, new_notes);
        }
    }
}

// 在 ShowNotes 中优先读取 core::Context，回退到 GlobalContext
std::vector<std::string> SessionManager::ShowNotes(const std::string& agent_name,
                                                    GlobalContext& global_ctx,
                                                    std::shared_ptr<core::Context> root_context) const {
    std::vector<std::string> notes;

    // 优先从 core::Context 读取
    if (root_context) {
        std::string var_key = "notes/" + agent_name;
        auto val = root_context->GetVar(var_key);
        if (val.has_value() && val->is_array()) {
            for (const auto& item : *val) {
                if (item.is_string()) notes.push_back(item.get<std::string>());
            }
            return notes;  // 有数据则直接返回
        }
    }

    // 回退到 GlobalContext（向后兼容）
    auto notes_opt = global_ctx.Read("memory/notes/" + agent_name);
    if (notes_opt && notes_opt->is_array()) {
        for (const auto& note : *notes_opt) {
            if (note.is_string()) notes.push_back(note.get<std::string>());
        }
    }
    return notes;
}
```

**相应修改头文件 `src/app/session.hpp`**：为 `AddNote` 和 `ShowNotes` 增加 `std::shared_ptr<core::Context>` 参数（默认 `nullptr` 保持兼容）。

在 `src/app/cli.cpp` 的 `RunChat` 中调用时传入 `root_context`。

**验收**：
- `/note add "test"` 后，`/note show` 正常显示
- 旧的 `~/.pu/context.json` 数据仍可读取
- 新数据同时写入 `~/.pu/contexts/active/root.json`（可通过查看文件确认）

**提交信息**：
```
refactor(context): dual-write notes/summary to core::Context

- Add core::Context write path in SessionManager::AddNote
- Prefer reading from core::Context in ShowNotes, fallback to GlobalContext
- Prepare for eventual removal of GlobalContext
```

---

### 步骤 5：目录重组（移动文件）

**目的**：使目录结构更符合逻辑，减少单文件目录。

**操作**：

```bash
# 5.1 移动 orchestrator 到 runtime
git mv src/orchestrator/orchestrator.cpp src/runtime/orchestrator.cpp
rmdir src/orchestrator  # 删除空目录

# 5.2 移动 renderer 到 app（仅源文件，头文件保持在 include/pu/ 便于包含）
git mv src/infra/renderer.cpp src/app/renderer.cpp
# 注意：include/pu/renderer.hpp 位置不变，但源文件移至 app/

# 5.3 重命名 agent_executor.cpp 为 executor.cpp（与头文件 executor.hpp 对齐）
git mv src/agent/agent_executor.cpp src/agent/executor.cpp
```

**更新 `CMakeLists.txt` 中的源文件路径**：

```cmake
# 原来的 orchestrator 行：
# src/orchestrator/orchestrator.cpp
# 改为：
src/runtime/orchestrator.cpp

# 原来的 renderer 行：
# src/infra/renderer.cpp
# 改为：
src/app/renderer.cpp

# 原来的 agent_executor.cpp：
# src/agent/agent_executor.cpp
# 改为：
src/agent/executor.cpp
```

**验收**：编译通过，`ctest` 全部通过。

**提交信息**：
```
refactor(dirs): reorganize source file locations

- Move orchestrator.cpp to runtime/
- Move renderer.cpp to app/
- Rename agent_executor.cpp to executor.cpp
- Remove empty orchestrator/ directory
```

---

### 步骤 6：CMake 库结构简化

**目的**：将 7 个库合并为 3 个核心库，减少链接复杂度。

**操作**：

重写 `CMakeLists.txt` 的库定义部分：

```cmake
# ===== 1. pu_core (基础库：HTTP + 平台 + Context + Backend 接口) =====
add_library(pu_core STATIC
    src/infra/curl_http_client.cpp
    src/infra/platform.cpp
    src/runtime/context.cpp
    src/runtime/delegation.cpp
    src/runtime/delegation_stack.cpp
    src/runtime/global_context.cpp
    src/runtime/orchestrator.cpp
    src/backends/ollama/ollama_backend.cpp
    src/backends/openai/openai_backend.cpp
    src/backends/common/streaming_json_parser.cpp
)
target_include_directories(pu_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_core PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
add_library(pu::core ALIAS pu_core)

# ===== 2. pu_agent (Agent + 工具 + 执行器) =====
add_library(pu_agent STATIC
    src/agent/agent_config.cpp
    src/agent/agent_manager.cpp
    src/agent/executor.cpp
    src/agent/agent_factory.cpp
    src/agent/tool_registry.cpp
    src/agent/llm_agent.cpp
    src/tools/command_executor.cpp
    src/tools/builtin_tools.cpp
    src/tools/python_tool.cpp
    src/conversation/conversation_store.cpp
)
target_include_directories(pu_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_agent PUBLIC pu_core nlohmann_json::nlohmann_json)
add_library(pu::agent ALIAS pu_agent)

# ===== 3. pu_app (CLI 主程序 + UI) =====
add_executable(pu
    src/app/main.cpp
    src/app/cli.cpp
    src/app/ui.cpp
    src/app/session.cpp
    src/app/renderer.cpp
)
target_include_directories(pu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(pu PRIVATE pu_agent pu_core)
```

**移除旧的库目标**：删除 `pu_backend_interface`、`pu_http`、`pu_platform`、`pu_backend`、`pu_context` 的相关定义。

**更新 `tests/CMakeLists.txt`**：

```cmake
target_link_libraries(pu_tests PRIVATE
    pu_agent pu_core
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)
```

**验收**：
- `cmake --build build` 成功，生成单一 `pu` 可执行文件
- 所有测试链接通过并运行成功
- 可执行文件功能与之前完全一致

**提交信息**：
```
build(cmake): consolidate libraries into pu_core, pu_agent, pu_app

- Merge pu_http, pu_platform, pu_context, pu_backend into pu_core
- Merge pu_backend_interface, pu_agent (old) into pu_agent
- Reduce 7 static libs to 2 static libs + 1 executable
- Simplify linking and reduce build overhead
```

---

### 步骤 7：更新文档（README / CONTRIBUTING）

**目的**：同步文档与最新架构。

**操作**：

**更新 `README.md`**：
- 补充 Context/Delegation 架构说明（参考 `docs/design/DEEPSEEK.md` 中的核心概念）
- 更新配置示例（指向 `agents.json`）
- 添加 `/push`、`/pop`、`/stack` 命令说明
- 添加 `PU_TRACE` 遥测使用指南

**更新 `CONTRIBUTING.md`**：
- 确保代码风格指引与当前一致（Google C++ Style，C++23）
- 补充新增模块（`core::Context`、`DelegationStack`）的测试要求
- 更新目录结构说明

**验收**：文档内容与代码实际结构一致，无过时引用。

**提交信息**：
```
docs: update README and CONTRIBUTING for current architecture

- Add Context/Delegation architecture overview
- Document /push, /pop, /stack CLI commands
- Update configuration examples
- Align contributing guidelines with current codebase
```

---

### 步骤 8：最终验证与覆盖率检查

**目的**：确保所有改动未引入回归，量化测试覆盖率。

**操作**：

```bash
# 8.1 清理并重新构建
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DCMAKE_CXX_FLAGS="--coverage"
cmake --build build -j$(nproc)

# 8.2 运行测试
ctest --test-dir build --output-on-failure

# 8.3 生成覆盖率报告（需安装 gcovr 或 lcov）
gcovr --root . --exclude "build" --exclude "tests" --html --output coverage_report.html
# 或使用 lcov:
lcov --capture --directory build --output-file coverage.info
genhtml coverage.info --output-directory coverage_report

# 8.4 手动功能验证
./build/pu ask --agent chat "Hello"
./build/pu chat --agent chat
# 在交互中测试: /note add test, /note show, /save, /push, /pop, /stack
```

**验收标准**：
- ✅ 全部原有测试通过（48+ 个）
- ✅ 无新增编译警告（`-Wall -Wextra`）
- ✅ 手动功能测试通过
- ✅ 覆盖率报告生成（可选，用于识别低覆盖模块）

**提交信息**：
```
test: final validation after code simplification

- All existing tests pass
- No new compiler warnings
- Manual smoke test passes for chat, notes, save/load, delegation
```

---

## 完整合并提交信息

在所有 8 个步骤完成后，向 `develop` 提交 PR 时使用以下汇总信息：

```
refactor: codebase simplification and health cleanup (8-step series)

Summary of changes:
- Remove dead code and duplicate headers (agent_config.hpp, src/config/)
- Unify exception handling with pu::Error base class
- Dual-write GlobalContext data to core::Context (backward compatible)
- Reorganize source directories (orchestrator → runtime, renderer → app)
- Consolidate CMake libraries from 7 to 3 (pu_core, pu_agent, pu_app)
- Update README and CONTRIBUTING for current architecture
- All tests pass; no functional regression

Prepares codebase for upcoming MCP client and Skills system integration.
```

---

## 回滚方案

如果某一步出现问题：

```bash
# 回滚到步骤开始前的状态（假设每步单独 commit）
git revert <commit-hash> --no-commit
git commit -m "revert: rollback step X"
```

或整体回滚到步骤 1 之前：
```bash
git checkout develop
git branch -D refactor/code-simplification
```

---

## 注意事项

1. **步骤 4 的双写迁移**：`GlobalContext` 仍被多处使用，仅迁移了笔记和摘要。后续版本可完全移除，本次不强制。
2. **步骤 6 的 CMake 合并**：如果编译出现循环依赖，检查 `pu_core` 是否包含了 `pu_agent` 的头文件（反之亦然），必要时调整依赖方向。
3. **步骤 5 的文件移动**：`git mv` 保留历史记录，普通 `mv` 会丢失历史，请务必使用 `git mv`。
4. **每步独立提交**：便于 code review 和问题定位。
