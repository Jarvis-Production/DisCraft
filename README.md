# DisCraft

Играй в **Dishonored** как игрок **Minecraft**. Ты двигаешься по физике Minecraft, у тебя
инвентарь и HUD Minecraft, ты ставишь и ломаешь блоки прямо в Дануолле и дерёшься со стражей
Dishonored оружием Minecraft, а она бьёт в ответ.

Ни одна из игр не переписывается. Minecraft крутит свою логику, Dishonored свой мир, NPC, ИИ,
миссии и сохранения. Плагин для Dishonored (`DisCraft.asi`) и Fabric-мод для Minecraft общаются
через общую память. Minecraft работает скрыто в фоне, а рисует всё Dishonored.

DisCraft построен по образцу [SkyCraft](https://github.com/chasmlol/SkyCraft) (тот же подход для
Skyrim, MIT). Fabric-мод и протокол взяты оттуда почти без изменений, а половина для Dishonored
написана заново под Unreal Engine 3.

> **Статус: ранний и экспериментальный.** Плагин написан без доступа к самой игре. Структуры
> движка он находит сам во время работы (см. «Как это устроено»), и эта часть проверена тестами на
> модели движка, но в настоящем Dishonored DisCraft ещё не запускался. Первый запуск почти наверняка
> потребует донастройки. Если что-то не так, пришли `%LOCALAPPDATA%\DisCraft\DisCraft.log`.
> Это фанатский проект, не связанный с Mojang, Microsoft, Arkane, Bethesda или ZeniMax. Для игры
> нужны обе игры.

## Что задумано и сделано

- **Движение.** Физика Minecraft на геометрии Dishonored: ходьба, бег, прыжки, приседание, падение.
  Форма мира Dishonored снимается трассировкой лучей по сетке вокруг игрока и передаётся в
  коллизию Minecraft: треугольники для игрока, воксели для мобов и предметов.
- **Блоки.** Ставь и ломай блоки где угодно. Они рисуются прямо в кадре Dishonored и прячутся за
  его стенами (через буфер глубины игры). Предметы, стрелы и рамка выделения тоже рисуются.
- **HUD и экраны Minecraft:** рука с предметом, хотбар, сердечки, голод, инвентарь, верстак,
  чат и меню поверх картинки Dishonored.
- **Бой.** Бей стражу, китобоев и крыс любым оружием Minecraft, включая луки, трезубцы и TNT.
  Урон уходит в `TakeDamage` самого персонажа, поэтому ИИ Dishonored поднимает тревогу и
  отвечает. Удары по тебе забирает Minecraft: работают броня, щит и тотемы, а умираешь ты по
  правилам Minecraft.
- **Взаимодействие.** **G** вызывает «использовать» Dishonored (двери, предметы, разговоры) на
  том, на что ты смотришь.
- **F11** временно возвращает управление Dishonored: способности, катсцены, всё, чего DisCraft не
  умеет. Повторное нажатие отдаёт его обратно Minecraft.
- **Мультиплеер (только сторона Minecraft):** друзья с DisCraft заходят в твой мир Minecraft через
  e4mc (`O` → «Открыть для сети», затем `/join <ссылка>`), как в SkyCraft.

## Чего пока нет

- Способности (Blink, Possession…) и оружие Dishonored, пока управляет Minecraft. Чтобы ими
  воспользоваться, нажми F11.
- Копания самого мира Dishonored. Ломать можно только поставленные блоки Minecraft (в SkyCraft
  копание есть, но для Dishonored нужно резать его геометрию).
- Камеры от третьего лица (F5 в Minecraft ничего не показывает) и плавания в воде Dishonored.
- Геймпада: только клавиатура и мышь.
- **Dishonored 2 и Death of the Outsider не поддерживаются.** Это другой, 64-битный движок (Void
  Engine). DisCraft сделан для **Dishonored (2012) / Definitive Edition на PC** (Unreal Engine 3,
  32 бит, DirectX 9).

## Требования

**Dishonored** — PC-версия Dishonored (2012) или Dishonored: Definitive Edition, работающая
на DirectX 9.

**Minecraft** — Minecraft: Java Edition 26.3 с Fabric. Готовая сборка DisCraft включает портативный
[Prism Launcher](https://prismlauncher.org/) с инстансом «DisCraft» (Minecraft 26.3, Fabric,
Fabric API, мод DisCraft). Prism сам скачивает Minecraft и Java после входа в аккаунт Microsoft,
которому принадлежит Minecraft: Java Edition. Можно указать и свой лаунчер (см. ниже).

Minecraft работает скрыто рядом с Dishonored. Нужно около 3 ГБ свободной оперативной памяти
сверх обычного.

### Про разные версии игр

Сам мод не проверяет никаких лицензий. Плагин не завязан на конкретный `Dishonored.exe`: адреса и
структуры движка он ищет во время работы, поэтому сборки из разных магазинов для него
одинаковы. Fabric-мод работает в любом экземпляре Minecraft 26.3 с Fabric, запущенном любым
лаунчером. Встроенный Prism Launcher входит через аккаунт Microsoft с купленным Minecraft (так
устроен сам Prism). DisCraft это не обходит.

## Установка

1. Скачай `DisCraft-<версия>.zip` (раздел Releases или артефакт сборки GitHub Actions
   `DisCraft-release`).
2. Распакуй его **в папку Dishonored** (ту, где лежит `Binaries`). Файлы лягут в
   `Binaries\Win32\`:
   - `DisCraft.asi`, `DisCraft.ini` — плагин и его настройки;
   - `d3d9.dll` — [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), который
     загружает `.asi`-плагины;
   - `DisCraft\DisCraft-Minecraft.zip` — Minecraft, который запускает плагин.
3. Запусти Dishonored как обычно. В первый раз DisCraft распакует свой Minecraft в
   `%LOCALAPPDATA%\DisCraft`, и маленькое окно Prism Launcher попросит войти в аккаунт Microsoft.
   Переключись на него (Alt+Tab), войди и вернись в игру. Prism скачает Minecraft, Fabric и Java
   (несколько минут, только в первый раз), а надпись в левом верхнем углу подскажет, что
   происходит.
4. Дальше всё автоматически: Minecraft стартует вместе с Dishonored без окна и звука, сам
   открывает мир «DisCraft», как только ты в уровне, и закрывается вместе с игрой.

**Если у тебя уже есть `d3d9.dll`** (например, ReShade) или плагин не загружается, переименуй
`d3d9.dll` из DisCraft в `dinput8.dll`, `ddraw.dll` или `winmm.dll`. Ultimate ASI Loader работает
под любым из этих имён.

**Удаление:** удали из `Binaries\Win32` файлы `DisCraft.asi`, `DisCraft.ini`, `d3d9.dll` и папку
`DisCraft`, затем папку `%LOCALAPPDATA%\DisCraft` (там Prism, твой вход в Microsoft, файлы
Minecraft и мир DisCraft).

### Свой лаунчер

В `DisCraft.ini`:

```ini
[Minecraft]
bStartWithGame = 1              ; 0: запускай Minecraft сам, как удобно
sLauncher =                     ; пусто: встроенный Minecraft; иначе полный путь к Prism, MultiMC или .bat
sArguments = --launch DisCraft  ; что передать лаунчеру
```

Твоему экземпляру нужны Minecraft 26.3, Fabric Loader 0.19.5+, Fabric API, Java 25 и
`discraft-fabric-<версия>.jar`, а в аргументах JVM `-Ddiscraft.startHidden=true`, если окно должно
быть скрыто с самого старта. Если Minecraft с модом уже запущен, второй не стартует.

## Управление

Почти всё забирает Minecraft. Клавиши Dishonored:

| Клавиша | Что делает |
|---|---|
| **Esc** | меню Dishonored (или закрывает открытый экран Minecraft) |
| **Tab**, **F9** | клавиши Dishonored (журнал, быстрая загрузка) |
| **G** | «использовать» Dishonored: двери, предметы, разговоры |
| **O** | меню паузы и настроек Minecraft |
| **F11** | вернуть управление Dishonored и обратно |
| **F10** | переключить режим глубины, если блоки видны сквозь стены или пропадают |

Остальное как в Minecraft: **E** инвентарь, **T** чат, **/** команды, **Shift** присесть, **Q**
выбросить, колесо мыши — хотбар. Список клавиш Dishonored меняется в `[Controls] sGameKeys`.

## Если что-то не так

- Лог: `%LOCALAPPDATA%\DisCraft\DisCraft.log`. Для отчёта об ошибке включи
  `[Debug] bDiagnostics = 1`.
- **«Unreal Engine data not found».** Плагин не нашёл таблицы движка. Пришли лог: в нём видно, что
  именно не нашлось. Любое смещение можно задать вручную в `[Engine]`.
- **Блоки видны сквозь стены или не видны вовсе.** Нажимай **F10** (три режима глубины). Если
  ни один не подходит, поменяй `[Render] fNearClipUU`.
- **Размер мира не тот** (ты слишком большой или маленький относительно дверей).
  `[World] fUnitsPerBlock` задаёт, сколько единиц Unreal в одном блоке (по умолчанию считается по
  росту персонажа).
- **Застрял на «starting Minecraft…».** Переключись на Prism (Alt+Tab): он может что-то качать,
  ждать входа или показывать ошибку. Лог Minecraft:
  `%LOCALAPPDATA%\DisCraft\Prism\instances\DisCraft\.minecraft\logs\latest.log`.

## Сборка из исходников

| Папка | |
|---|---|
| `native/` | плагин для Dishonored (C++20, Win32 x86): `DisCraft.asi` |
| `fabric/` | мод для Minecraft (Java 25, Fabric, Minecraft 26.3) |
| `protocol/` | раскладка общей памяти, общая для обеих сторон |
| `tools/` | упаковщик релиза, заглушка игры для тестов (`fake_dishonored.py`), бандл Prism |
| `docs/DESIGN.md` | как всё устроено |

Плагин (нужна **32-битная** сборка):

```bat
:: Windows, Visual Studio 2022+
cmake -S native -B build\native -A Win32
cmake --build build\native --config Release
```

```sh
# Linux / MSYS2, MinGW-w64
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/native/cmake/mingw-i686.cmake
cmake --build build/native
# юнит-тесты (координаты, мешер коллизий, поиск структур UE3 на модели движка):
cmake -S native -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
```

Мод Minecraft: `cd fabric && ./gradlew build`. Нужен JDK 25. Для разработки есть
`./gradlew runClient` — тестовый Minecraft, который не закрывается вместе с игрой. Без Dishonored его
можно гонять через `python tools/fake_dishonored.py` (только Windows).

Релиз: `python tools/package.py --build` кладёт `DisCraft-<версия>.zip` и
`discraft-fabric-<версия>.jar` в `dist/`. Сторонние загрузки закреплены по версии и хэшу.

GitHub Actions (`.github/workflows/build.yml`) на каждый push собирает обе половины, плагин
дважды (MinGW и MSVC), гоняет тесты и выкладывает готовый архив как артефакт.

## Как это устроено (коротко)

- **Без SDK и жёстких адресов.** Dishonored работает на 32-битном Unreal Engine 3. Плагин находит
  таблицу имён (`GNames`) и массив объектов (`GObjects`) по их содержимому, затем сам вычисляет
  раскладку `UObject`/`UStruct`/`UProperty`/`UFunction` по известным вещам: у `Core.Object.Vector`
  поля X, Y, Z лежат на 0, 4 и 8, `Engine.Actor` наследует `Core.Object` и так далее. Дальше всё
  делается через рефлексию движка по именам.
- **Кадр игры.** `UFunction::Func` у `PlayerController.PlayerTick` подменяется на свой обработчик.
  Это и точка входа в игровой поток, и способ найти `ProcessEvent`, через который вызываются
  `SetLocation`, `Trace`, `TakeDamage` и т.п.
- **Мир для Minecraft** снимается вертикальными лучами `Actor.Trace` по сетке ¼ блока вокруг
  игрока, а мешер превращает попадания в треугольники и воксели.
- **Картинка.** Хук `IDirect3DDevice9::Present`. Блоки рисуются фиксированным конвейером D3D9 с
  камерой игры, оверлей Minecraft накладывается поверх.

Подробно в [docs/DESIGN.md](docs/DESIGN.md).

## Лицензия

[MIT](LICENSE). Основано на SkyCraft (MIT), подробности в [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
