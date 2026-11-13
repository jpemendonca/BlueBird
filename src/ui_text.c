#include "ui_text.h"

#ifdef __PROSPERO__
int sceSystemServiceParamGetInt(int param_id, int *value);
#define SYSTEM_PARAM_LANG 1
#endif

static const char *const k_codes[UI_LANG_COUNT] = { "en", "pt", "es", "fr", "de", "ru", "ja" };

static const char *const k_text[UI_LANG_COUNT][UI_COUNT] = {
    [UI_EN] = {
        [UI_RUNNING] = "BlueBird %s running. Plug in a USB Bluetooth adapter",
        [UI_RUNNING_IP] = "BlueBird %s running. Panel: Media tab, or http://%s:%d on your phone",
        [UI_ADAPTER_READY] = "BlueBird: Bluetooth adapter ready (%s)",
        [UI_NO_ADAPTER] = "BlueBird: no USB Bluetooth adapter found. Plug one into the PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: the Bluetooth adapter did not start. Unplug it and plug it in again",
        [UI_SPEAKER_PLAYING] = "BlueBird: playing on %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s disconnected",
    },
    [UI_PT] = {
        [UI_RUNNING] = "BlueBird %s ativo. Plugue um adaptador Bluetooth USB",
        [UI_RUNNING_IP] = "BlueBird %s ativo. Painel: aba Midia, ou http://%s:%d no celular",
        [UI_ADAPTER_READY] = "BlueBird: adaptador Bluetooth pronto (%s)",
        [UI_NO_ADAPTER] = "BlueBird: nenhum adaptador Bluetooth USB encontrado. Plugue um no PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: o adaptador Bluetooth nao iniciou. Tire e plugue de novo",
        [UI_SPEAKER_PLAYING] = "BlueBird: tocando em %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s desconectou",
    },
    [UI_ES] = {
        [UI_RUNNING] = "BlueBird %s activo. Conecta un adaptador Bluetooth USB",
        [UI_RUNNING_IP] = "BlueBird %s activo. Panel: pestaña Multimedia, o http://%s:%d en el móvil",
        [UI_ADAPTER_READY] = "BlueBird: adaptador Bluetooth listo (%s)",
        [UI_NO_ADAPTER] = "BlueBird: no se encontró ningún adaptador Bluetooth USB. Conecta uno a la PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: el adaptador Bluetooth no arrancó. Desconéctalo y vuelve a conectarlo",
        [UI_SPEAKER_PLAYING] = "BlueBird: sonando en %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s se desconectó",
    },
    [UI_FR] = {
        [UI_RUNNING] = "BlueBird %s actif. Branchez un adaptateur Bluetooth USB",
        [UI_RUNNING_IP] = "BlueBird %s actif. Panneau : onglet Multimédia, ou http://%s:%d sur votre téléphone",
        [UI_ADAPTER_READY] = "BlueBird : adaptateur Bluetooth prêt (%s)",
        [UI_NO_ADAPTER] = "BlueBird : aucun adaptateur Bluetooth USB trouvé. Branchez-en un sur la PS5",
        [UI_ADAPTER_FAILED] = "BlueBird : l'adaptateur Bluetooth n'a pas démarré. Débranchez-le puis rebranchez-le",
        [UI_SPEAKER_PLAYING] = "BlueBird : lecture sur %s",
        [UI_SPEAKER_LOST] = "BlueBird : %s déconnecté",
    },
    [UI_DE] = {
        [UI_RUNNING] = "BlueBird %s läuft. Stecke einen USB-Bluetooth-Adapter ein",
        [UI_RUNNING_IP] = "BlueBird %s läuft. Übersicht: Medien-Tab oder http://%s:%d auf dem Handy",
        [UI_ADAPTER_READY] = "BlueBird: Bluetooth-Adapter bereit (%s)",
        [UI_NO_ADAPTER] = "BlueBird: kein USB-Bluetooth-Adapter gefunden. Stecke einen in die PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: der Bluetooth-Adapter ist nicht gestartet. Abziehen und wieder einstecken",
        [UI_SPEAKER_PLAYING] = "BlueBird: Wiedergabe auf %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s getrennt",
    },
    [UI_RU] = {
        [UI_RUNNING] = "BlueBird %s запущен. Подключите USB-адаптер Bluetooth",
        [UI_RUNNING_IP] = "BlueBird %s запущен. Панель: вкладка «Медиа» или http://%s:%d на телефоне",
        [UI_ADAPTER_READY] = "BlueBird: адаптер Bluetooth готов (%s)",
        [UI_NO_ADAPTER] = "BlueBird: USB-адаптер Bluetooth не найден. Подключите его к PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: адаптер Bluetooth не запустился. Отключите и подключите его снова",
        [UI_SPEAKER_PLAYING] = "BlueBird: звук на %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s отключено",
    },
    [UI_JA] = {
        [UI_RUNNING] = "BlueBird %s 実行中。USB Bluetooth アダプターを接続してください",
        [UI_RUNNING_IP] = "BlueBird %s 実行中。パネル: メディアタブ、またはスマホで http://%s:%d",
        [UI_ADAPTER_READY] = "BlueBird: Bluetooth アダプターの準備ができました (%s)",
        [UI_NO_ADAPTER] = "BlueBird: USB Bluetooth アダプターが見つかりません。PS5 に接続してください",
        [UI_ADAPTER_FAILED] = "BlueBird: Bluetooth アダプターが起動しませんでした。抜いてから挿し直してください",
        [UI_SPEAKER_PLAYING] = "BlueBird: %s で再生中",
        [UI_SPEAKER_LOST] = "BlueBird: %s の接続が切れました",
    },
};

ui_lang_t ui_lang_from_system(int system_lang)
{
    switch (system_lang) {
    case 0:                     return UI_JA;
    case 2: case 22:            return UI_FR;   /* France, Canada */
    case 3: case 20:            return UI_ES;   /* Spain, Latin America */
    case 4:                     return UI_DE;
    case 7: case 17:            return UI_PT;   /* Portugal, Brazil */
    case 8:                     return UI_RU;
    default:                    return UI_EN;
    }
}

static ui_lang_t console_lang(void)
{
    static int lang = -1;
#ifdef __PROSPERO__
    int system_lang;
    if (lang < 0) lang = sceSystemServiceParamGetInt(SYSTEM_PARAM_LANG, &system_lang) == 0
                         ? ui_lang_from_system(system_lang) : UI_EN;
#else
    if (lang < 0) lang = UI_EN;
#endif
    return (ui_lang_t)lang;
}

const char *ui_text_in(ui_lang_t lang, ui_msg_t msg)
{
    return k_text[lang][msg];
}

const char *ui_text(ui_msg_t msg)
{
    return k_text[console_lang()][msg];
}

const char *ui_lang_code(void)
{
    return k_codes[console_lang()];
}
