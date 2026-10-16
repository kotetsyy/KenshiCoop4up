## Прямой IP без ключей и приглашений

- **UDP теперь подключается только по IP:port.** Убраны обязательные индивидуальные приглашения, ключи, пароли, профиль допуска и ручные allow-листы.
- **Упрощено окно F2:** хост задаёт порт, клиент — адрес с портом. Удалены Create invite / Copy invite и дополнительное поле приглашения.
- Удалены генератор приглашений и прежняя модификация authenticated-IP. Используется исходная закреплённая версия GameNetworkingSockets; ENet не возвращается.
- Сохранены шифрование GNS, отдельный сетевой worker, лимит **HOST + 3 JOIN**, проверки пакетов и готовность конкретной загрузки мира. Steam-аутентификация сохранена; скрытого переключения со Steam на UDP нет.
- Обновлены native ABI **3**, UI API **4** и протоколы: публичный Release — **82**, приватный Harness — **83**.

## Установка и совместимость

**Скачивайте [`KenshiCoop-kit.zip`](https://github.com/kotetsyy/KenshiCoop4up/releases/download/v0.2.1/KenshiCoop-kit.zip).** Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов. Нужен RE_Kenshi. Сохраните конфиг и сейвы.

**Переход с v0.2.0 и 0.1.x — вручную полным комплектом на всех машинах.** Старый updater не принимает новый ABI. Не смешивайте DLL разных версий: v0.2.1 несовместима с прежними протоколами.

В архиве ровно шесть файлов: `KenshiCoop.dll`, `KenshiCoopUI.dll`, `KenshiCoopNetSteam.dll`, `KenshiCoopNetUDP.dll`, `KenshiCoop.mod` и `RE_Kenshi.json`. Отдельные DLL в релизе предназначены для автообновления уже совместимой версии; для ручной установки используйте ZIP.

## Важно про открытый UDP

Любой, кто может достучаться до UDP-порта хоста, сможет занять свободное место. Личность UDP-участника не проверяется; шифрование не обеспечивает защиту от подмены посредником. Если нужна подтверждённая Steam identity, выбирайте Steam. Для подключения через интернет по прямому IP по-прежнему может требоваться проброс порта; обход NAT/CGNAT не добавлен.

## Оставшиеся игровые проблемы

Этот выпуск убирает UDP-допуск, но не объявляет устранёнными расхождения процедурной обстановки в новых областях, предметов окружения, выходных контейнеров добычи, замков и вражеских NPC. Исправление торговли из v0.2.0 сохранено; все игровые границы stack/split/cancel/reconnect и начальная экипировка торговца ещё не сертифицированы. Повторные проверки спальников и остальных семи пунктов ведутся отдельно.

<details><summary>English</summary>

Direct UDP now uses only IP:port: no keys, passwords, per-client invitations, admission profiles or allow-lists. F2 no longer shows invitation controls. GNS encryption, HOST + 3 JOIN, world-load readiness and Steam identity authentication remain; there is no ENet or silent transport fallback.

**Install the complete six-file `KenshiCoop-kit.zip` manually on every machine with Kenshi closed when upgrading from v0.2.0 or 0.1.x.** Previous updaters reject ABI3. Release protocol82 and Harness protocol83 are incompatible with older builds and with each other. Preserve config and saves; do not mix DLLs.

Reachable UDP peers are not identity-authenticated and can occupy free seats. Encryption does not provide MITM resistance. Internet port forwarding may still be necessary. This release does not certify the remaining gameplay desynchronization reports fixed.

</details>
