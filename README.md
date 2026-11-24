# Streetless

StreetPass через Интернет для Nintendo 3DS. Сервис не зависит от NetPass, Pretendo, DNS на консоли или UDP. Общий зал ограничен тремя входящими случайными слотами за 24 часа; приватный режим оставляет обмен с друзьями и комнатами по приглашению. Отправка из кабинета исполняется при следующей синхронизации отправителя на 3DS.

## Сервер

Нужен Go 1.26+. Соберите бинарник: `cd server && go build ./cmd/streetlessd`. Сгенерируйте пароль администратора и сохраните отдельно хеш:

```sh
umask 077
ADMIN_PASSWORD=$(openssl rand -hex 32)
printf '%s\n' "$ADMIN_PASSWORD" > admin-password.txt
printf '%s' "$ADMIN_PASSWORD" | openssl dgst -sha256 -r | cut -d ' ' -f1 > admin.sha256
unset ADMIN_PASSWORD
```

Перенесите пароль в менеджер секретов; не храните пароль, ключ TLS или SQLite в репозитории. Для публичного сервера укажите реальный DNS-домен, публичный IPv4 и сертификат с полной цепочкой до ISRG Root X1:

```sh
export STREETLESS_ORIGIN=https://streetless.example.org
export STREETLESS_PUBLIC_IPV4=203.0.113.10  # заменить на адрес сервера или прокси
export STREETLESS_ADMIN_HASH_FILE=/etc/streetless/admin.sha256
export STREETLESS_TLS_CERT=/etc/streetless/tls.crt
export STREETLESS_TLS_KEY=/etc/streetless/tls.key
export STREETLESS_DB=/var/lib/streetless/streetless.sqlite
./streetlessd
```

По умолчанию сервис слушает HTTPS на `0.0.0.0:443`; для самого бинарника используйте [systemd unit](deploy/streetless.service) с отдельным пользователем `streetless`. `/healthz` показывает готовность SQLite. Сайт `/`, личный кабинет `/login`, панель управления `/admin` (пользователь `admin`). Администратор выдаёт каждому игроку `config.txt` для 3DS и отдельный ключ сайта. При отзыве участника его сервисные ключи и очередь удаляются; уже полученное игрой нельзя отозвать.

### Reverse proxy

На одном хосте используйте [пример nginx](deploy/nginx/streetless.conf.example) и [systemd drop-in](deploy/systemd/streetless.service.d/proxy.conf). В `server.env` задайте `STREETLESS_TRANSPORT=proxy` и `STREETLESS_LISTEN=127.0.0.1:8080`, а `STREETLESS_ORIGIN` и `STREETLESS_PUBLIC_IPV4` оставьте внешними адресами. TLS завершается на nginx. Не публикуйте backend-порт, не монтируйте приложение под URL-префиксом. Nginx передаёт исходный `Host`, а Streetless не доверяет `Forwarded` или `X-Forwarded-*`. При прокси на другом хосте оставьте backend в `direct` с TLS, ограничьте порт firewall на адрес прокси и настройте проверку сертификата upstream.

## Клиент 3DS

На Luma3DS с актуальным системным ПО включите StreetPass в игре, например Mii Plaza. Соберите клиент командой `make -C client` в devkitPro с `3ds-dev`, `3ds-curl`, `3ds-jansson`. Либо из корня:

```sh
docker run --rm -v "$PWD:/work" -w /work \
  devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 \
  sh -lc 'dkp-pacman --noconfirm --needed -S 3ds-dev 3ds-curl 3ds-jansson && make -C client'
```

Поместите `client/streetless.3dsx` и выданный `config.txt` в `/3ds/streetless/` на SD-карте. Запустите из Homebrew Launcher и выберите **Sync**. `host` в конфиге обязан совпадать с именем в сертификате; `ipv4` применяется для соединения без DNS. Для локального тестового CA пересоберите с `make -C client CA_FILE=/absolute/path/to/root.pem`.

### Тестовая доставка

1. Тестовый получатель включает «Принимать тестовые проходы от администратора» в кабинете или **Admin test passes** на 3DS.
2. На тестовом отправителе постройте отдельный `make -C client TEST_SLOT_EXPORT=1`; запустите `streetless-slot-export.3dsx` и экспортируйте слот включённой игры в `/3ds/streetless/test-XXXXXXXX.slot`. Экспорт не отправляет данные по сети и не включает выдачу ключей.
3. В `/admin` загрузите этот файл для выбранного получателя. После Sync на его консоли результат обрабатывается обычным путём: успешный локальный commit → ACK, подтверждённо плохой формат/HMAC → reject. Полный inbox и временные ошибки оставляют доставку в очереди. Тест не создаёт обратной доставки и не расходует публичный лимит; срок 24 часа.

## Проверка

- `cd server && go test ./...` — SQLite, маршрутизация, TLS proxy, сессии и multipart-отправка из админки.
- В корне: `cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/wire.c client/tests/wire_test.c -o /tmp/streetless-wire-test && /tmp/streetless-wire-test`.
- Аппаратная проверка требует двух консолей с уникальными Mii. Серверные тесты не подтверждают, что Mii появился в Mii Plaza или что соединение доступно у конкретного провайдера.

CECD-обёртки основаны на [NetPass](https://gitlab.com/3ds-netpass/netpass) и распространяются под GPL-3.0-or-later; см. [COPYING](COPYING) и уведомления в исходниках.
