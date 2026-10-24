# NpcTouch для Fallout 4 VR

Мод физического взаимодействия с NPC в духе PLANCK из Skyrim VR. Это надстройка над [ROCK](https://github.com/brunocatani/ROCK): ROCK отвечает за физику рук, а NpcTouch решает, как NPC реагирует на касание.

## Текущий шаг: лог касаний

Пока мод ничего не меняет в игре. Он только замечает, когда твоя рука касается NPC, и:

- пишет это в лог `Documents\My Games\Fallout4VR\F4SE\NpcTouch.log`;
- на 3 секунды показывает в шлеме отладочную надпись вида `NpcTouch: Right hand -> Piper`.

Это проверка того, что ROCK присылает нам события касаний. Все следующие функции (реакции, эмоции, толчки) будут построены на них.

## Требования

- Fallout 4 VR 1.2.72, F4SE VR, VR Address Library
- FRIK 0.79.1 или новее
- ROCK последней версии (0.92+). Коллизия рук с NPC в ROCK включена по умолчанию.

## Установка

1. Открой вкладку **Actions** этого репозитория, выбери последнюю успешную сборку (зелёная галочка) и скачай архив **NpcTouch** внизу страницы.
2. Распакуй его в папку `Data` игры (или установи архив через менеджер модов). Должен получиться файл `Data\F4SE\Plugins\NpcTouch.dll`.
3. Запусти игру, подойди к любому NPC и дотронься до него рукой.
4. Пришли файл `NpcTouch.log`.

## Сборка вручную

Нужны Visual Studio 2022, CMake 4.2+ и vcpkg (переменная `VCPKG_ROOT`).

```powershell
git clone --recursive https://github.com/<логин>/fo4vr-npc-touch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Лицензия

GPL-3.0. Код подключения к ROCK основан на примерах из [RPS SDK](https://github.com/brunocatani/RPS_SDK).
