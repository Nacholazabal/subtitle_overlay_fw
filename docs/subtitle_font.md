# Fuente de subtítulos

El firmware usa **Lato Semibold a 34 px**, rasterizada offline como una máscara monocromática. En ejecución no se carga un archivo TTF ni se depende de FreeType: el binario contiene los glifos y sus métricas proporcionales en `subtitle_font.c`.

La fuente incluye ASCII imprimible y los caracteres españoles `¡¿ÁÉÍÑÓÚÜáéíñóúü`. El normalizador conserva esos caracteres en UTF-8 y convierte comillas tipográficas, guiones Unicode y puntos suspensivos a equivalentes soportados. Los caracteres no disponibles se sustituyen por un espacio; una búsqueda directa de un glifo desconocido usa `?` como respaldo.

Cada glifo almacena su bitmap 1-bpp, desplazamiento respecto de la línea base y avance horizontal obtenido de las métricas `hmtx` de la fuente. El ajuste de línea se calcula en píxeles y las palabras demasiado anchas se dividen con guion. Se muestran como máximo tres líneas: una para el segmento final anterior y dos para el segmento actual. El segmento actual parcial conserva la distinción visual mediante tramado; los segmentos finales se dibujan sólidos.

El rectángulo negro se adapta al contenido renderizado, con 18 px de margen horizontal y 10 px vertical. Se centra horizontalmente y conserva el margen inferior dependiente de la resolución. Como el hardware ofrece un único rectángulo, un subtítulo de varias líneas comparte un solo fondo compacto.

## Regeneración

La fuente se regenera de forma determinista con:

```sh
python3 tools/fontgen/generate_subtitle_font.py
```

El generador usa `/usr/share/fonts/truetype/lato/Lato-Semibold.ttf` por defecto y acepta `--font` y `--output`. La licencia SIL Open Font License se conserva en `third_party/lato/OFL.txt`.
La salida predeterminada siempre se resuelve desde la raíz del repositorio, sin
depender del directorio desde el que se ejecute el comando. El archivo generado
registra el nombre y el SHA-256 de la fuente, pero no una ruta local de la
máquina que lo produjo.

Después de regenerar se deben ejecutar las pruebas y el cross-build habituales:

```sh
make test
./scripts/build.sh
```
