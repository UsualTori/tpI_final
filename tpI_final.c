/*  Maximo Toranzo 1R7 418617 Tp integrador
    
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>

// Arregle llamada api: Curlib error 60.
// Arregle error video: Elimine la line que resivia la img sinquerer.
// Arregle UTF-8: Enviaba /uxxxx en vez del caracter.
// Arregle bug almacenar memoria: char *msg intentaba guardar mas de lo que podia.
// Arregle error file: me equiboque al mandar la direccion del archibo -1h de vida.
// wasa

// Se asigna al leer el token del archivo indicado en argumentos[1].
static const char *TOKEN_BOT;
// El registro se guarda en el directorio desde donde se ejecuta el programa.
#define ARCHIVO_LOG "registro_mensajes.txt"

char *llamar_api(const char *, const char *);// Prototipo de la funcion para llamar a la API de Telegram

struct memoria{
    char *data;
    size_t size;
};

// Escapa saltos y separadores para que cada mensaje ocupe una sola linea.
static int escribir_campo_log(FILE *archivo, const char *texto){
    const unsigned char *p = (const unsigned char *)texto;

    while (*p) {
        const char *escape = NULL;
        switch (*p) {
            case '\\': escape = "\\\\"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            case '|': escape = "\\|"; break;
        }
        if (escape) {
            if (fputs(escape, archivo) == EOF) return 0;
        } else if (fputc(*p, archivo) == EOF) {
            return 0;
        }
        p++;
    }
    return 1;
}

// Agrega una linea con el formato: hora Unix | nombre | mensaje.
static void registrar_mensaje(long long fecha, const char *nombre,
                              const char *mensaje){
    FILE *archivo = fopen(ARCHIVO_LOG, "a");
    int error = 0;

    if (!archivo) {
        fprintf(stderr, "No se pudo abrir %s para registrar mensajes.\n",
                ARCHIVO_LOG);
        return;
    }

    if (fprintf(archivo, "%lld | ", fecha) < 0 ||
        !escribir_campo_log(archivo, nombre) ||
        fputs(" | ", archivo) == EOF ||
        !escribir_campo_log(archivo, mensaje) ||
        fputc('\n', archivo) == EOF) {
        error = 1;
    }
    if (fclose(archivo) != 0) error = 1;
    if (error) {
        fprintf(stderr, "No se pudo escribir correctamente en %s.\n",
                ARCHIVO_LOG);
    }
}

// Convierte un codigo Unicode y agrega sus bytes UTF-8 al texto de destino.
// capacidad es el tamano de destino e indice indica donde continuar escribiendo.
static int agregar_unicode_utf8(char *destino, size_t capacidad, size_t *indice, unsigned long codigo){
    unsigned char bytes[3];
    size_t cantidad;

    // NUL terminaria la cadena antes de tiempo; un sustituto requiere su par.
    if (codigo == 0 || (codigo >= 0xd800 && codigo <= 0xdfff)) return 0;

    // ASCII usa un byte UTF-8 sin cambios.
    if (codigo <= 0x7f) {
        bytes[0] = (unsigned char)codigo;
        cantidad = 1;
    // Hasta 0x7ff se representa con dos bytes: prefijos 110 y 10.
    } else if (codigo <= 0x7ff) {
        bytes[0] = (unsigned char)(0xc0 | (codigo >> 6));
        bytes[1] = (unsigned char)(0x80 | (codigo & 0x3f));
        cantidad = 2;
    // Los demas caracteres de un escape \uXXXX valido usan tres bytes.
    } else {
        bytes[0] = (unsigned char)(0xe0 | (codigo >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codigo >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (codigo & 0x3f));
        cantidad = 3;
    }

    // Reserva un byte para el '\0' final y evita escribir fuera del buffer.
    if (*indice >= capacidad || cantidad > capacidad - *indice - 1)
        return 0;
    memcpy(destino + *indice, bytes, cantidad);
    // Actualiza la posicion para que la proxima parte se escriba despues.
    *indice += cantidad;
    return 1;
}

// Decodifica una cadena JSON
char *decodificar_cadena_json(char *texto){
    const char *p = texto;
    while(*p){// Direccion del fin de la cadena
        if(*p == '\\'){
            p +=2;           // Salta el caracter escapado
        }else if(*p == '"'){
            break;          // Se encontro el cierre de la cadena
        }else
            p++;// Avanza al siguiente caracter
    }
    if(*p != '"') return NULL; // No se encontro el cierre de la cadena

    size_t cap = (size_t)(p - texto) + 1; // Capacidad necesaria para la cadena decodificada
    char *destino = malloc(cap);
    if(!destino) return NULL; // Error de memoria

    const char *origen = texto;
    size_t i = 0;
    while(*origen && *origen != '"'){
        char c;
        if(*origen != '\\'){
            c = *origen++; // Salta el caracter de escape
        }else{
            origen++;
            switch(*origen){
                case 'n': c ='\n';origen++; break;
                case 't': c ='\t';origen++; break;
                case 'r': c ='\r';origen++; break;
                case '"': c ='"';origen++; break;
                case 'u': {
                    unsigned long codigo;
                    int digitos_leidos = 0;

                    // origen apunta a 'u'; avanza al primer digito hexadecimal.
                    origen++;
                    // %4lx lee como maximo cuatro hexadecimales; %n cuenta cuantos leyo.
                    if (sscanf(origen, "%4lx%n", &codigo, &digitos_leidos) != 1 || digitos_leidos != 4 || !agregar_unicode_utf8(destino, cap, &i, codigo)) {
                        free(destino);
                        return NULL;
                    }
                    // Sigue decodificando despues de los cuatro digitos.
                    origen += digitos_leidos;
                    continue;
                }
                default: free(destino); return NULL;
            }
        }
        destino[i++] = c;
    }
    destino[i] = '\0'; // Asegura el final de la cadena
    return destino;
}

// Envia un mensaje a un chat de Telegram
void enviar_mensaje(long long chat_id, const char *nombre, const char *textIn){
    char url[2048];
    char textOut[1024];

    // Genera la respuesta segun el mensaje recibido.
    if (strcasecmp(textIn, "hola") == 0) {
        snprintf(textOut, sizeof(textOut), "Hola %s!", nombre);
    } else if (strcasecmp(textIn, "chau") == 0) {
        snprintf(textOut, sizeof(textOut), "Chau %s!", nombre);
    } else if (textIn) {
        snprintf(textOut, sizeof(textOut), "%s",textIn);
    }

    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", TOKEN_BOT);

    // Codifica el texto para enviarlo de forma segura en el cuerpo POST
    CURL *curl_enc = curl_easy_init();
    if (!curl_enc) {
        fprintf(stderr, "Error al inicializar codificacion\n");
        return;
    }
    char *text_encoded = curl_easy_escape(curl_enc, textOut, 0);
    curl_easy_cleanup(curl_enc);
    if (!text_encoded) {
        fprintf(stderr, "Error al codificar el texto\n");
        return;
    }

    size_t post_tam = strlen(text_encoded) + 64;
    char *post = malloc(post_tam);
    if (!post) {
        curl_free(text_encoded);
        fprintf(stderr, "Error de memoria al preparar el envio\n");
        return;
    }

    snprintf(post, post_tam, "chat_id=%lld&text=%s", chat_id, text_encoded);
    curl_free(text_encoded);

    char *respuesta = llamar_api(url, post);
    free(post);

    if (respuesta) {
        if (strstr(respuesta, "\"ok\":true")) {
            printf("Mensaje enviado: %s\n", textOut);
            // Solo registra el mensaje saliente cuando Telegram confirma el envio.
            registrar_mensaje((long long)time(NULL), "Bot", textOut);
        } else {
            fprintf(stderr, "Telegram rechazo el mensaje: %s\n", respuesta);
        }
        free(respuesta);
    } else {
        printf("Error al enviar\n");
    }
}

void enviar_imagen(long long chat_id, const char *imagen, const char *text){
    char url[2048];

    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", TOKEN_BOT);

    // Codifica los datos para enviarlos de forma segura en el cuerpo POST (x-www-form-urlencoded).
    CURL *curl_enc = curl_easy_init();
    if (!curl_enc) {
        fprintf(stderr, "Error al inicializar codificacion\n");
        return;
    }

    char *text_encoded = curl_easy_escape(curl_enc, text, 0);
    char *img_encoded = curl_easy_escape(curl_enc, imagen, 0);

    curl_easy_cleanup(curl_enc);

    if (!img_encoded || !text_encoded) {
        fprintf(stderr, "Error al codificar los datos de la imagen\n");
        curl_free(img_encoded);
        curl_free(text_encoded);
        return;
    }

    size_t post_tam = strlen(img_encoded) + strlen(text_encoded) + 64;
    char *post = malloc(post_tam);
    if (!post) {
        curl_free(img_encoded);
        curl_free(text_encoded);
        fprintf(stderr, "Error de memoria al preparar el envio\n");
        return;
    }

    snprintf(post, post_tam, "chat_id=%lld&photo=%s&caption=%s", chat_id, img_encoded, text_encoded);
    curl_free(img_encoded);
    curl_free(text_encoded);

    // POST: post_data no es NULL.
    char *respuesta = llamar_api(url, post);
    free(post);

    if (respuesta) {
        if (strstr(respuesta, "\"ok\":true")) {
            printf("Imagen enviada: %s\ninfo: %s\n", imagen, text);
            // Guarda el texto de la imagen enviada con la hora actual.
            registrar_mensaje((long long)time(NULL), "Bot", text);
        } else {
            fprintf(stderr, "Telegram rechazo la imagen: %s\n", respuesta);
        }
        free(respuesta);
    } else {
        printf("Error al enviar\n");
    }
}

void procesar_NASA(const char *json, long long chat_id){
    char *title = strstr(json, "\"title\":\"");
    title += 9; // Mueve el puntero al inicio del titulo
    title = decodificar_cadena_json(title); // Decodifica el titulo

    char *link  = strstr(json, "\"permalink\":\"");
    link += 13; // Mueve el puntero al inicio del enlace
    link = decodificar_cadena_json(link); // Decodifica el enlace

    char *img  = strstr(json, "\"hdurl\":\"");
    img += 9; // Mueve el puntero al inicio del enlace
    img = decodificar_cadena_json(img); // Decodifica el enlace

    char *msg = malloc(strlen(title) + strlen(link) + 12);
    sprintf(msg, "%s\nmas info: %s", title, link);

    enviar_imagen(chat_id, img, msg);
    
    free(img);
    free(title);
    free(link);
    free(msg);
}

//procesesar las respuestas de la API
long long procesar_Telegram(const char *json, long long offset){
    char *ID = strstr(json, "\"update_id\":");
    if(!ID) return offset; // No se encontro update_id, no hay actualizaciones nuevas
    long long update_id = atoll(ID + 12); // Extrae el nuevo update_id
    printf("Update ID: %lld\n", update_id); // Muestra el update_id en la consola

    char *ChatID = strstr(json, "\"chat\":{\"id\":");
    if(!ChatID) return update_id + 1; // No se encontro chat_id
    long long chat_id = atoll(ChatID + 13); // Extrae el chat_id
    printf("Chat ID: %lld\n", chat_id); // Muestra el chat_id en la consola

    // Telegram entrega la hora Unix del mensaje en el campo "date".
    char *Date = strstr(json, "\"date\":");
    long long fecha = Date ? atoll(Date + 7) : (long long)time(NULL);

    // Se usa el nombre recibido en el JSON del usuario.
    char *Name = strstr(json, "\"first_name\":\"");
    Name += 14;
    Name = decodificar_cadena_json(Name);
    printf("Nombre del usuario: %s\n", Name);

    char *Text = strstr(json, "\"text\":\"");
    if(!Text) {
        // Registra tambien mensajes entrantes que no contienen texto.
        registrar_mensaje(fecha, Name, "[mensaje sin texto]");
        free(Name);
        return update_id + 1;
    }
    Text += 8; // Mueve el puntero al inicio del contenido del texto
    Text = decodificar_cadena_json(Text); // Decodifica el texto del mensaje
    printf("Texto recibido: %s\n", Text); // Muestra el texto recibido en la consola
    // Registra la entrada antes de procesarla o responderla.
    registrar_mensaje(fecha, Name, Text);

    if (strcmp(Text, "/nasa") == 0){
        char *nasa_json = llamar_api("https://science.nasa.gov/wp-json/wp/v2/apod-basic/?api_key=DEMO_KEY", NULL);
        procesar_NASA(nasa_json, chat_id); // Procesa la respuesta de la API de NASA
        free(nasa_json);
    }else {
        enviar_mensaje(chat_id, Name, Text); // Envia el mensaje de vuelta al usuario
    }

    free(Text);
    free(Name);
    return update_id + 1; // Actualiza el offset para la proxima llamada a getUpdates
}



// Callback para manejar la respuesta de la API de Telegram
size_t callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memoria *mem = (struct memoria *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

// Funcion para llamar a la API y obtener la respuesta
char* llamar_api(const char *url, const char *post_data) {
    CURL *curl = curl_easy_init();
    struct memoria mem = {0};
    char error_buffer[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&mem);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);// Usa certificados de CA nativos del sistema operativo

    if (post_data) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    }

    CURLcode res = curl_easy_perform(curl);
    // Muestra el codigo y el mensaje de error en la salida de errores.
    if (res != CURLE_OK) {
        fprintf(stderr, "Error Codigo: %d: %s\n", res,
                error_buffer[0] ? error_buffer : curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(mem.data);
        return NULL;
    }

    curl_easy_cleanup(curl);
    return mem.data;
}

int main(int cantidad_argumentos, char *argumentos[]){
    long long offset = 0; // Offset para obtener actualizaciones desde Telegram
    char URL[1024] = {0}; // Buffer para construir la URL de la API
    char token[512];
    FILE *archivo_token;

    // Se espera un unico argumento: la ruta del archivo que contiene el token.
    if (cantidad_argumentos != 2) {
        fprintf(stderr, "Aviso: debes indicar el archivo que contiene el token.\n");
        fprintf(stderr, "Uso: %s <archivo_token>\n", argumentos[0]);
        return 1;
    }

    // argumentos[1] contiene la ruta pasada al iniciar el programa.
    archivo_token = fopen(argumentos[1], "r");
    if (!archivo_token) {
        fprintf(stderr, "No se pudo abrir el archivo del token: %s\n", argumentos[1]);
        return 1;
    }
    // Lee la primera linea del archivo, donde debe estar el token.
    if (!fgets(token, sizeof(token), archivo_token)) {
        fprintf(stderr, "El archivo del token esta vacio o no se pudo leer.\n");
        fclose(archivo_token);
        return 1;
    }
    // Rechaza una linea que supere el tamano del buffer del token.
    if (!strchr(token, '\n') && !feof(archivo_token) && fgetc(archivo_token) != EOF) {
        fprintf(stderr, "El token del archivo es demasiado largo.\n");
        fclose(archivo_token);
        return 1;
    }
    fclose(archivo_token);

    // Quita el salto de linea del archivo antes de usar el token en la URL.
    token[strcspn(token, "\r\n")] = '\0';
    // No se puede iniciar si el archivo no contenia un token.
    if (token[0] == '\0') {
        fprintf(stderr, "El archivo del token no contiene un token valido.\n");
        return 1;
    }
    // El buffer vive durante todo main, asi que el puntero sigue siendo valido.
    TOKEN_BOT = token;

    while(1){
        // Construye la consulta a Telegram con el token y el ultimo offset.
        snprintf(URL, sizeof(URL),"https://api.telegram.org/bot%s/getUpdates?offset=%lld",TOKEN_BOT, offset);
        char *json = llamar_api(URL, NULL);
        if(json){
            printf("JSON recibido: %s\n", json); // Muestra el JSON recibido en la consola
            offset = procesar_Telegram(json, offset);
            free(json);
        }

        sleep(2);
        //break; // Salir del bucle en este ejemplo
    }
    return 0;
}
