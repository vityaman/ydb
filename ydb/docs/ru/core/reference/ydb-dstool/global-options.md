# Глобальные параметры

Глобальные параметры являются общими для всех подкоманд утилиты {{ ydb-short-name }} DSTool.

Параметр | Описание
---|---
`-?`, `-h`, `--help` | Вывести встроенную справку.
`-v`, `--verbose` | Получить подробный вывод при выполнении команды.
`-q`, `--quiet` | Подавить вывод несущественных сообщений при выполнении команды.
`-n`, `--dry-run` | Выполнить пробный запуск команды.
`-e`, `--endpoint` | Эндпоинт для подключения к кластеру {{ ydb-short-name }} в формате `[PROTOCOL://]HOST[:PORT]`.<br/>Значения по умолчанию: протокол — `http`, порт — `8765`.
`--grpc-port` | gRPC-порт для вызова процедур.
`--mon-port` | Порт для просмотра данных HTTP-мониторинга в формате JSON.
`--mon-protocol` | Если протокол для подключения к кластеру не задан явно в эндпоинте, то используется указанное здесь значение.
`--token-file` | Путь к файлу с [Access Token](../../security/authentication.md#iam).
`--ca-file` | Путь к файлу корневого PEM-сертификата для TLS-соединения.
`--http` | Использовать HTTP для подключения к Blob Storage вместо gRPC.
`--http-timeout` | Тайм-аут на операции ввода-вывода сокета во время HTTP(s)-запросов.
`--insecure` | Разрешить небезопасную передачу данных по HTTPS. В этом режиме не проверяются SSL-сертификат и имя хоста.
