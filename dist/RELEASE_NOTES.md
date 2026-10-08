## Исправление подключения и компактная установка

В публичную сборку включён текущий нативный F2-интерфейс и исправления передачи мира из dev-сборки.

- Исправлен доступ к сейвам по кириллическим путям, например `C:\Users\Миша`: перечисление, чтение, запись, проверка содержимого и перенос папок используют Unicode API Windows.
- Тайм-аут записи мира больше не считается успешным сохранением. Нечитаемый bootstrap не отправляет пустой `LOAD_GO`; при невозможности начать передачу сессия завершается с ошибкой.
- Приём BEGIN/FILE/DONE происходит одним атомарным срезом очереди, чтобы последний блок файла не терялся перед проверкой CRC.
- Сохранены редактируемые поля ника и адреса, кириллица, Ctrl+V, Tab, выбор хоста/клиента и Steam/UDP, реальные этапы подключения и прогресс передачи мира.
- `KenshiCoop-kit.zip` содержит только **четыре файла** в папке `KenshiCoop`: `KenshiCoop.dll`, `KenshiCoopUI.dll`, `KenshiCoop.mod`, `RE_Kenshi.json`. Без лаунчеров, готового конфига, README, PROVENANCE и вложенного архива исходников.

**Установка на обеих машинах:** полностью закройте Kenshi, скопируйте папку `KenshiCoop` из `KenshiCoop-kit.zip` в `<Kenshi>\mods\` с заменой файлов. RE_Kenshi должен быть установлен; включите KenshiCoop в меню Mods. Обе DLL берите из одного архива.

Настройки задаются в F2. `coop_config.json` создаётся при их сохранении; существующий конфиг не удаляйте и не заменяйте. `PROVENANCE.json` не требуется ни публичной, ни dev-сборке для запуска. Запускайте игру обычным способом, через Steam или `kenshi_x64.exe`.

**Сборка:** Release и Harness, обе пары core/UI, успешно собраны с v100. По просьбе пользователя игровые тесты этой версии не запускались; проверка подключения между двумя ПК остаётся за пользователем. Публичная и приватная диагностическая сборки несовместимы друг с другом.

Соответствующие исходники по AGPL-3.0 — отдельный файл `KenshiCoop-source.zip` в этом релизе. В папку игры его копировать не нужно.

**Известное ограничение:** ранее выход через WM_CLOSE давал `0xC0000409` в RE_Kenshi. RE_Kenshi не изменён; исправление этого внешнего сбоя не заявляется.

<details><summary>English</summary>

The public build includes the current native F2 UI and the save-transfer fixes from the development build. Save filesystem operations now use Unicode Windows APIs, including Cyrillic profile paths. A save timeout is no longer treated as successful completion; an unreadable bootstrap never announces an empty LOAD_GO, and a refused transfer ends the session with an error. BEGIN/FILE/DONE reception uses one atomic queue snapshot.

KenshiCoop-kit.zip contains only four runtime files inside KenshiCoop/: the core DLL, companion UI DLL, mod and RE_Kenshi JSON. No launcher, shipped config, README, provenance or nested source archive. Close Kenshi on both machines, copy KenshiCoop/ into mods/, replace both DLLs together, and preserve your existing coop_config.json. F2 creates it when saving settings. RE_Kenshi is still required. Launch normally through Steam or kenshi_x64.exe.

Both Release and Harness core/UI pairs build successfully with v100. In-game tests of this version were skipped at the user's request; two-machine connectivity is not claimed verified. Public and private diagnostic builds cannot connect to each other. Corresponding AGPL-3.0 sources are a separate KenshiCoop-source.zip asset. The previously observed RE_Kenshi exit crash is not claimed fixed.

</details>
