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
        [UI_ADAPTER_FOUND] = "BlueBird: Bluetooth adapter found (%s), starting it",
        [UI_ADAPTER_READY] = "BlueBird: Bluetooth adapter ready (%s)",
        [UI_NO_ADAPTER] = "BlueBird: no USB Bluetooth adapter found. Plug one into the PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: the Bluetooth adapter did not start. Unplug it and plug it in again",
        [UI_ADAPTER_GONE] = "BlueBird: the Bluetooth adapter was unplugged or stopped answering",
        [UI_CONNECTING] = "BlueBird: connecting to %s",
        [UI_PAIRED] = "BlueBird: paired with %s",
        [UI_SPEAKER_PLAYING] = "BlueBird: playing on %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s disconnected",
        [UI_CONNECT_FAILED] = "BlueBird: %s did not connect. See the panel for why",
        [UI_CAPTURE_FAILED] = "BlueBird: the console's audio could not be captured. The test tone still works",
        [UI_STOPPED] = "BlueBird stopped",
        [UI_CRASH] = "BlueBird stopped by an error (signal %d). Send the log from the panel",
    },
    [UI_PT] = {
        [UI_RUNNING] = "BlueBird %s ativo. Plugue um adaptador Bluetooth USB",
        [UI_RUNNING_IP] = "BlueBird %s ativo. Painel: aba Midia, ou http://%s:%d no celular",
        [UI_ADAPTER_FOUND] = "BlueBird: adaptador Bluetooth encontrado (%s), iniciando",
        [UI_ADAPTER_READY] = "BlueBird: adaptador Bluetooth pronto (%s)",
        [UI_NO_ADAPTER] = "BlueBird: nenhum adaptador Bluetooth USB encontrado. Plugue um no PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: o adaptador Bluetooth nao iniciou. Tire e plugue de novo",
        [UI_ADAPTER_GONE] = "BlueBird: o adaptador Bluetooth foi tirado ou parou de responder",
        [UI_CONNECTING] = "BlueBird: conectando em %s",
        [UI_PAIRED] = "BlueBird: pareado com %s",
        [UI_SPEAKER_PLAYING] = "BlueBird: tocando em %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s desconectou",
        [UI_CONNECT_FAILED] = "BlueBird: %s nao conectou. Veja o motivo no painel",
        [UI_CAPTURE_FAILED] = "BlueBird: nao deu para capturar o audio do console. O tom de teste continua funcionando",
        [UI_STOPPED] = "BlueBird parou",
        [UI_CRASH] = "BlueBird parou por um erro (sinal %d). Mande o log pelo painel",
    },
    [UI_ES] = {
        [UI_RUNNING] = "BlueBird %s activo. Conecta un adaptador Bluetooth USB",
        [UI_RUNNING_IP] = "BlueBird %s activo. Panel: pestaña Multimedia, o http://%s:%d en el móvil",
        [UI_ADAPTER_FOUND] = "BlueBird: adaptador Bluetooth encontrado (%s), iniciándolo",
        [UI_ADAPTER_READY] = "BlueBird: adaptador Bluetooth listo (%s)",
        [UI_NO_ADAPTER] = "BlueBird: no se encontró ningún adaptador Bluetooth USB. Conecta uno a la PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: el adaptador Bluetooth no arrancó. Desconéctalo y vuelve a conectarlo",
        [UI_ADAPTER_GONE] = "BlueBird: el adaptador Bluetooth se desconectó o dejó de responder",
        [UI_CONNECTING] = "BlueBird: conectando con %s",
        [UI_PAIRED] = "BlueBird: emparejado con %s",
        [UI_SPEAKER_PLAYING] = "BlueBird: sonando en %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s se desconectó",
        [UI_CONNECT_FAILED] = "BlueBird: %s no se conectó. Mira el motivo en el panel",
        [UI_CAPTURE_FAILED] = "BlueBird: no se pudo capturar el audio de la consola. El tono de prueba sigue funcionando",
        [UI_STOPPED] = "BlueBird se detuvo",
        [UI_CRASH] = "BlueBird se detuvo por un error (señal %d). Envía el registro desde el panel",
    },
    [UI_FR] = {
        [UI_RUNNING] = "BlueBird %s actif. Branchez un adaptateur Bluetooth USB",
        [UI_RUNNING_IP] = "BlueBird %s actif. Panneau : onglet Multimédia, ou http://%s:%d sur votre téléphone",
        [UI_ADAPTER_FOUND] = "BlueBird : adaptateur Bluetooth trouvé (%s), démarrage",
        [UI_ADAPTER_READY] = "BlueBird : adaptateur Bluetooth prêt (%s)",
        [UI_NO_ADAPTER] = "BlueBird : aucun adaptateur Bluetooth USB trouvé. Branchez-en un sur la PS5",
        [UI_ADAPTER_FAILED] = "BlueBird : l'adaptateur Bluetooth n'a pas démarré. Débranchez-le puis rebranchez-le",
        [UI_ADAPTER_GONE] = "BlueBird : l'adaptateur Bluetooth a été débranché ou ne répond plus",
        [UI_CONNECTING] = "BlueBird : connexion à %s",
        [UI_PAIRED] = "BlueBird : appairé avec %s",
        [UI_SPEAKER_PLAYING] = "BlueBird : lecture sur %s",
        [UI_SPEAKER_LOST] = "BlueBird : %s déconnecté",
        [UI_CONNECT_FAILED] = "BlueBird : %s ne s'est pas connecté. Voir la raison dans le panneau",
        [UI_CAPTURE_FAILED] = "BlueBird : impossible de capturer le son de la console. Le son de test fonctionne toujours",
        [UI_STOPPED] = "BlueBird arrêté",
        [UI_CRASH] = "BlueBird arrêté par une erreur (signal %d). Envoyez le journal depuis le panneau",
    },
    [UI_DE] = {
        [UI_RUNNING] = "BlueBird %s läuft. Stecke einen USB-Bluetooth-Adapter ein",
        [UI_RUNNING_IP] = "BlueBird %s läuft. Übersicht: Medien-Tab oder http://%s:%d auf dem Handy",
        [UI_ADAPTER_FOUND] = "BlueBird: Bluetooth-Adapter gefunden (%s), wird gestartet",
        [UI_ADAPTER_READY] = "BlueBird: Bluetooth-Adapter bereit (%s)",
        [UI_NO_ADAPTER] = "BlueBird: kein USB-Bluetooth-Adapter gefunden. Stecke einen in die PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: der Bluetooth-Adapter ist nicht gestartet. Abziehen und wieder einstecken",
        [UI_ADAPTER_GONE] = "BlueBird: der Bluetooth-Adapter wurde abgezogen oder antwortet nicht mehr",
        [UI_CONNECTING] = "BlueBird: verbinde mit %s",
        [UI_PAIRED] = "BlueBird: gekoppelt mit %s",
        [UI_SPEAKER_PLAYING] = "BlueBird: Wiedergabe auf %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s getrennt",
        [UI_CONNECT_FAILED] = "BlueBird: %s hat sich nicht verbunden. Grund in der Übersicht",
        [UI_CAPTURE_FAILED] = "BlueBird: der Ton der Konsole konnte nicht aufgenommen werden. Der Testton funktioniert weiter",
        [UI_STOPPED] = "BlueBird beendet",
        [UI_CRASH] = "BlueBird durch einen Fehler beendet (Signal %d). Sende das Protokoll aus der Übersicht",
    },
    [UI_RU] = {
        [UI_RUNNING] = "BlueBird %s запущен. Подключите USB-адаптер Bluetooth",
        [UI_RUNNING_IP] = "BlueBird %s запущен. Панель: вкладка «Медиа» или http://%s:%d на телефоне",
        [UI_ADAPTER_FOUND] = "BlueBird: найден адаптер Bluetooth (%s), запуск",
        [UI_ADAPTER_READY] = "BlueBird: адаптер Bluetooth готов (%s)",
        [UI_NO_ADAPTER] = "BlueBird: USB-адаптер Bluetooth не найден. Подключите его к PS5",
        [UI_ADAPTER_FAILED] = "BlueBird: адаптер Bluetooth не запустился. Отключите и подключите его снова",
        [UI_ADAPTER_GONE] = "BlueBird: адаптер Bluetooth отключён или не отвечает",
        [UI_CONNECTING] = "BlueBird: подключение к %s",
        [UI_PAIRED] = "BlueBird: сопряжено с %s",
        [UI_SPEAKER_PLAYING] = "BlueBird: звук на %s",
        [UI_SPEAKER_LOST] = "BlueBird: %s отключено",
        [UI_CONNECT_FAILED] = "BlueBird: %s не подключилось. Причина — в панели",
        [UI_CAPTURE_FAILED] = "BlueBird: не удалось захватить звук консоли. Тестовый сигнал работает",
        [UI_STOPPED] = "BlueBird остановлен",
        [UI_CRASH] = "BlueBird остановлен из-за ошибки (сигнал %d). Отправьте журнал из панели",
    },
    [UI_JA] = {
        [UI_RUNNING] = "BlueBird %s 実行中。USB Bluetooth アダプターを接続してください",
        [UI_RUNNING_IP] = "BlueBird %s 実行中。パネル: メディアタブ、またはスマホで http://%s:%d",
        [UI_ADAPTER_FOUND] = "BlueBird: Bluetooth アダプターを検出しました (%s)。起動中",
        [UI_ADAPTER_READY] = "BlueBird: Bluetooth アダプターの準備ができました (%s)",
        [UI_NO_ADAPTER] = "BlueBird: USB Bluetooth アダプターが見つかりません。PS5 に接続してください",
        [UI_ADAPTER_FAILED] = "BlueBird: Bluetooth アダプターが起動しませんでした。抜いてから挿し直してください",
        [UI_ADAPTER_GONE] = "BlueBird: Bluetooth アダプターが抜かれたか、応答しなくなりました",
        [UI_CONNECTING] = "BlueBird: %s に接続中",
        [UI_PAIRED] = "BlueBird: %s とペアリングしました",
        [UI_SPEAKER_PLAYING] = "BlueBird: %s で再生中",
        [UI_SPEAKER_LOST] = "BlueBird: %s の接続が切れました",
        [UI_CONNECT_FAILED] = "BlueBird: %s に接続できませんでした。理由はパネルを確認してください",
        [UI_CAPTURE_FAILED] = "BlueBird: 本体の音声をキャプチャできませんでした。テスト音は使えます",
        [UI_STOPPED] = "BlueBird を停止しました",
        [UI_CRASH] = "BlueBird がエラーで停止しました (シグナル %d)。パネルからログを送ってください",
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
