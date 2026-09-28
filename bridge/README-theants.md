# Dataflow bridge для theants

`dataflow_bridge.hpp` — мост между theants и Dataflow C++ SDK. Честно о
терминологии: для статически слинкованного MSVC-проекта «zero-
instrumentation» (инъекция без правок кода) не работает, поэтому мост
сведёт инструментацию к **одной строке на точку входа** — остальное
(RAII-завершение, шифрование, батчинг, отправка) делает SDK.

## Подключение

1. Скопировать в theants:
   - `dataflow-cpp/include/dataflow.hpp` → `theants/third_party/dataflow/dataflow.hpp`
   - `dataflow-cpp/bridge/dataflow_bridge.hpp` → `theants/third_party/dataflow/dataflow_bridge.hpp`
   - `dataflow-cpp/src/dataflow.cpp` → `theants/third_party/dataflow/dataflow.cpp` (добавить в `.vcxproj`)
2. Собрать. На Windows SDK использует WinHTTP + BCrypt — никаких новых
   зависимостей и `.lib` кроме системных (`winhttp.lib` прилинкуется сам
   через `#pragma comment`).
3. Запуск с окружением:

```bat
set DATAFLOW_ENDPOINT=http://localhost:25080
set DATAFLOW_API_KEY=df_...
set DATAFLOW_SERVICE_NAME=theants
set DATAFLOW_ENCRYPTION_KEY=<секрет, только у вас>
```

Без этих переменных SDK пассивен — строки в коде можно не убирать.

## Точки интеграции (по файлам theants)

### `Core/HttpServer.cpp` — HTTP API

Каждый роут — точка `DATAFLOW_REQUEST_BEGIN("/route")`, в конце хендлера —
`DATAFLOW_REQUEST_END(status)`:

```cpp
// в начале диспетчера запросов:
DATAFLOW_REQUEST_BEGIN((std::string("/api/") + route).c_str());
...
DATAFLOW_REQUEST_END(status_code);
```

Внутри хендлеров полезно: `DATAFLOW_DATA("query", queryStr)` — payload
уйдёт зашифрованным, а имя поля попадёт в lineage и PII-панель.

### `Core/Tick.cpp` — игровой цикл

```cpp
void Tick() {
    DATAFLOW_SPAN("Core.Tick");
    DATAFLOW_DATA_NUM("entities", entity_count);
    ...
}
```

Сэмплирование: `DATAFLOW_SAMPLE_RATIO=0.05` в env, чтобы тики не заливали
проект (долгие/ошибочные тики и так попадут в 30-дневный tail-ретеншн).

### `Core/McpServer.cpp`, `Core/TaskQueue.cpp` — фоновые операции

```cpp
DATAFLOW_SPAN("Mcp.Handle");
DATAFLOW_SPAN("TaskQueue.Drain");
DATAFLOW_ERROR("queue overflow");   // текущий (внутренний) спан
```

## Что вы получите в дашборде

- **Traces** — цепочки HTTP-запрос → Tick → подоперации с длительностями;
- **Errors** — группировка `DATAFLOW_ERROR`;
- **Data flow graph** — рёбра между модулями theants (пакеты берутся из
  имён спанов `Module.Function`);
- **Sensitive/PII** — клиентская классификация полей payload'а
  (`DATAFLOW_DATA("email", ...)` пометится как `email`) с E2E-шифрованием
  значений;
- **Environments** — ОС/версия/CPU theants в панели сервисов.

## Проверка без theants

Готовый пример того же моста: `example-cpp/` (fleet-api) — поднимается
через `docker compose up demo-cpp`.
