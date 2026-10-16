## Новая сетевая инфраструктура

Это крупный переход со старой сетевой ветки, а не только исправление торговли.

- **Полностью переработан сетевой транспорт:** старая реализация на ENet заменена новой native-инфраструктурой на GameNetworkingSockets / SteamNetworkingSockets. Steam и прямой IP используют отдельные модули `KenshiCoopNetSteam.dll` и `KenshiCoopNetUDP.dll`.
- **Обновлены сетевой протокол и формат обмена.** Публичная версия использует протокол **80** и не соединяется со старыми сборками 0.1.x.
- Сеть обслуживается отдельным native worker. Целевая частота публикации снимков сущностей — **100 Гц**; захват состояния зависит от FPS игры. Это не обещание пинга 1 мс и не перевод всех каналов на 100 Гц.
- Steam и UDP выбираются явно. Ошибка подключения через Steam **не запускает скрытый переход на UDP**.
- **Steam:** ручное добавление SteamID гостей в allow-лист не требуется. **UDP:** индивидуальные приглашения остаются обязательными — хост создаёт отдельное приглашение каждому клиенту. Подключение только по IP:port без приглашения **в этой версии не реализовано**; требование убрать этот ручной допуск пока не выполнено.

## Исправления

### Торговля

- Исправлен односторонний обмен изменениями: клиентские покупки и продажи теперь доходят до HOST, а результат сделки передаётся участникам сессии.
- Исправлено **перетаскивание товаров** при покупке и продаже. Рабочий способ покупки по ПКМ сохранён.
- Передаётся выбранное количество товара, а не безусловно один предмет. Итог сделки и фактически переданное количество определяет HOST.
- Исправлена обработка предмета на курсоре и штатного списания/зачисления денег: защита от повторной оплаты и потери остатка стопки при подтверждении сделки.

### Перенос игрока и спальники

- Исправлен самопроизвольный подъём игрока через несколько секунд после того, как другой игрок перенёс его и уложил на спальник. Это исправление состояния персонажа; оно не означает устранение всех расхождений самих кроватей между HOST и JOIN.

### Подключение и загрузка мира

- Исправлены ошибки доставки Steam callbacks и времени жизни сетевых сообщений в пути подключения.
- Убран преждевременный перезапуск ещё живой native-попытки подключения на старой двухсекундной границе.
- Усилен барьер готовности: **READY относится к конкретной текущей загрузке мира**, а не к любому ранее полученному подтверждению. Завершение передачи сохранения само по себе не означает готовность проверяемого участка.
- Сохранение мира для подключения клиента больше не сбрасывает пользовательскую паузу хоста. Ожидание загрузки и выбранная игроком пауза учитываются отдельно.

### Автообновление

- Новый updater устанавливает **все четыре DLL одной сборки**, включая UI и оба сетевых модуля, с проверкой SHA-256 и транзакционной заменой.
- Исправлена гонка, при которой updater менял файлы одновременно с загрузкой UI или сетевой DLL.
- После установки обновления новое подключение требует перезапуска Kenshi, чтобы не смешивать уже загруженный код со следующим комплектом DLL.

## Синхронизация мира: внесённые изменения и оставшиеся проблемы

По следующим направлениям код изменён, но **полное устранение исходных игровых багов пока не заявляется**:

- **Процедурная обстановка и кровати:** добавлены отслеживание происхождения объектов, авторитетные снимки HOST по участкам и эпохам, согласование JOIN и подтверждение результата перед готовностью участка. Полный жизненный цикл в новых областях, переподключение и повторная загрузка ещё требуют игрового подтверждения.
- **Предметы окружения:** реализован отдельный механизм согласования. Разные предметы на столах и полках не объявлены исправленными; непроверенные категории массово не включены.
- **Выходные контейнеры добычи:** изменены обработка изъятия и согласование содержимого. Исчезновение меди у всех участников, одновременный подбор и отсутствие повторного получения на финальной сборке ещё требуют подтверждения.
- **Замки:** доработаны передача авторитетного состояния и обработка выхода участника. Поведение после переподключения и выгрузки/повторной загрузки участка не считается окончательно проверенным.
- **Вражеские NPC:** исправлена конкретная ошибка учёта времени получения сброса боевого состояния. Все причины периодического расхождения позиций и боевых анимаций этим не закрыты.

Начальные различия ассортимента, отсутствующая экипировка торговца, потери при прямом обмене, другие рассинхроны времени, координат и игровых состояний остаются отдельными проблемами. Исправление сделок с торговцем не означает устранение всех этих случаев.

## Совместимость

Всем игрокам нужна одна версия. Публичный Release использует **протокол 80**, приватный Harness — **81**; они несовместимы. В Harness автообновление отключено.

Полноценная Steam P2P-сессия между двумя реальными аккаунтами и реальные сессии на 3–4 игроков ещё не подтверждены в полном объёме.

## Установка и обновление

Скачайте **[`KenshiCoop-kit.zip`](https://github.com/kotetsyy/KenshiCoop4up/releases/download/v0.2.0/KenshiCoop-kit.zip)**. Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов. RE_Kenshi должен быть установлен. Сохраните свой `coop_config.json` и сейвы.

**Первый переход с 0.1.x обязателен вручную, полным комплектом:** старый updater не поддерживает новую схему. В архиве ровно шесть необходимых файлов: `KenshiCoop.dll`, `KenshiCoopUI.dll`, `KenshiCoopNetSteam.dll`, `KenshiCoopNetUDP.dll`, `KenshiCoop.mod` и `RE_Kenshi.json`.

После перехода новый updater заменяет все четыре DLL для следующего запуска. `KenshiCoop.mod` и `RE_Kenshi.json` устанавливаются из полного комплекта.

<details><summary>English</summary>

### Network infrastructure

The old ENet transport has been replaced by a reworked native GameNetworkingSockets / SteamNetworkingSockets infrastructure with separate Steam and direct-UDP modules. The new public protocol **80** is incompatible with 0.1.x and private Harness protocol **81**. Networking runs on a dedicated worker; entity snapshot publication targets 100 Hz, while fresh state capture still depends on game FPS. There is no silent Steam-to-UDP fallback. Steam does not require manually adding guest SteamIDs. Direct UDP still requires a separate host-generated invitation for each client; IP:port-only joining without an invitation is not implemented in this release. Removing that manual admission requirement remains unfinished.

### Fixes

- Client merchant purchases/sales now reach HOST and distribute the authoritative result. Drag-and-drop trading supports the selected quantity, with cursor-stack and native payment protection; right-click trading is retained.
- Fixed the player standing up automatically shortly after another player carries them onto a bedroll.
- Fixed Steam callback delivery and network-message lifetime errors, and premature restarts of still-live native connection attempts.
- READY is tied to the current world load and required scope reconciliation, not just save transfer. Preparing the join save no longer clears the host's user pause.
- The updater installs all four matching DLLs transactionally. Runtime image loading is coordinated with replacement; a new connection requires a restart after an update.

### Remaining work

Procedural scope snapshots/reconciliation, environment items, resource containers, locks and NPC combat-state handling have received changes, but their full gameplay lifecycles are not certified fixed. Initial merchant stock/outfit differences, direct-trade item loss and other time/position desyncs remain separate issues. Actual two-account Steam P2P and real 3–4-player sessions are not fully verified.

### Installation

The first upgrade from 0.1.x must be manual: close Kenshi and install the six files from **`KenshiCoop-kit.zip`** into `<Kenshi>/mods/KenshiCoop/`. RE_Kenshi is required. Preserve config and saves. Subsequent updates replace all four DLLs together; `.mod` and `RE_Kenshi.json` come from the full kit. Harness auto-updates are disabled.

</details>
