# Ingnomia — русский форк

Форк [Ingnomia](https://github.com/rschurade/Ingnomia) 0.9.0 с русским интерфейсом
и игровыми текстами. Игровая логика не менялась: новых механик и сетевой игры нет,
это одиночная игра для Windows.

Оригинальное описание проекта, инструкции по разработке и правила участия от
авторов Ingnomia — в [docs/README.upstream.md](docs/README.upstream.md)
(на английском).

## Что переведено

- все 1 227 строк таблицы `Translation` и тексты событий в базе данных;
- все 251 строка интерфейса (`content/xaml/localization/ru_RU.xaml`) и надписи
  прямо в XAML-страницах;
- строки, которые собирает код: дата, сведения о клетке, соседи, торговля,
  названия королевств, имена по умолчанию для складов и зон.

Имена гномов остались латиницей — это собственные имена из оригинала.
Подробности и исключения — в `localization/ru_RU/NOTES.md`, термины — в
`localization/ru_RU/GLOSSARY.md`.

## Сборка для Windows

Нужны Windows, Visual Studio 2022, Qt 6.9, NoesisGUI 3.0.12, Steamworks SDK и
OpenAL — те же зависимости, что у оригинала. В GitHub Actions сборка
запускается вручную (workflow «CMake», кнопка Run workflow):

- `evaluation` — без лицензионного ключа Noesis; NoesisGUI работает в
  ознакомительном режиме и сам показывает об этом предупреждение;
- `licensed` — с ключом из секретов репозитория `NOESIS_LICENSE_NAME` и
  `NOESIS_LICENSE_KEY`.

После сборки CI отдельно запускает собранную игру с программным OpenGL и
сохраняет снимок окна и журналы (артефакт runtime-proof). Это проверка того,
что окно игры открылось, а не тест игрового процесса.

Для локальной сборки без ключа добавьте к CMake `-DINGNOMIA_NOESIS_EVALUATION=ON`.
Без этого параметра пустой ключ, как и раньше, останавливает компиляцию.

## Архив для игроков

    python3 tools/package/make_windows_package.py --mode install --install-dir install \
        --asset-zip Ingnomia-0.9.0-windows-x64.zip --out dist

Быстрый вариант без пересборки берёт оригинальный Ingnomia.exe 0.9.0 и
кладёт поверх русские данные (`--mode release --release-zip ...`). В нём
часть строк из кода остаётся английской — об этом написано в
«ПРОЧТИ МЕНЯ.txt» внутри архива.

Обе команды сверяют официальный ZIP с размером и SHA-256 из
`packaging/windows/upstream-release.json`.

## Лицензия

GNU AGPL 3.0, как у оригинала (файл `LICENSE`). Авторы Ingnomia — Ralph
Schurade и команда Ingnomia.
