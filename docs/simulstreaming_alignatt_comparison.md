# SimulStreaming y AlignAtt frente al pipeline STT actual

Fecha de elaboración: 2026-07-16  
Estado: análisis técnico y propuesta experimental; todavía no constituye una decisión de migración.

## Propósito

Este documento compara el pipeline de subtitulado incremental actualmente implementado en
`subtitle_overlay_fw` con una alternativa basada en **SimulStreaming y AlignAtt**. El objetivo es
dejar asentados:

- el origen académico de las dos estrategias;
- qué problema intenta resolver cada una;
- qué partes del sistema actual se conservarían;
- qué evidencia existe a favor de SimulStreaming;
- qué limitaciones tiene esa evidencia para nuestro caso;
- y cómo realizar una comparación experimental justa antes de decidir una migración.

La pregunta no es solamente cuál sistema transcribe más rápido. Para el overlay importan, de
manera simultánea, la exactitud, la latencia hasta una aparición útil, la estabilidad del texto,
la entrega al firmware y la legibilidad en pantalla.

## Resumen ejecutivo

El sistema actual convierte Whisper, un modelo originalmente offline, en un servicio incremental
mediante ventanas de audio, VAD, hipótesis parciales, finales y un filtro simple de acuerdo entre
prefijos. Utiliza `faster-whisper/CTranslate2`, que ha demostrado buen rendimiento de inferencia
en Colab y ya está integrado con el bridge, el protocolo de ACK de la placa y el banco de pruebas.

SimulStreaming propone otra política de emisión. Ejecuta Whisper con PyTorch y utiliza
**AlignAtt**, que consulta las alineaciones presentes en la atención cruzada del modelo para
evitar emitir tokens asociados con el borde más reciente —y por lo tanto más incierto— del
audio. Incorpora además procesamiento de habla continua, contexto entre buffers, VAC, beam
search y simulación con o sin costo computacional.

La propuesta es **evaluar SimulStreaming en paralelo**, primero sobre archivos y luego, sólo si
el resultado lo justifica, conectarlo al mismo protocolo live. No se reemplazará inicialmente el
pipeline actual. De esta forma, el sistema vigente permanece como baseline reproducible y la
alternativa puede descartarse sin afectar el firmware.

## Conceptos y nomenclatura

### Whisper no es un modelo streaming nativo

Whisper fue diseñado para procesar audio con contexto disponible, no para emitir de forma causal
una palabra apenas se pronuncia. Los sistemas estudiados lo convierten en incremental ejecutando
inferencia repetidamente a medida que llega audio y aplicando una **política de emisión** que
decide qué texto es suficientemente seguro para entregar.

### Partial, final y commit

- Un **partial** es una hipótesis todavía revisable.
- Un **final** indica que el sistema cerró un segmento, normalmente por silencio o límite de
  buffer.
- Un **commit** es una decisión más fuerte: cierto prefijo se considera confirmado y ya no debe
  modificarse aunque llegue más audio.

En un overlay, `partial` y `final` no alcanzan por sí solos para describir la experiencia visual.
También importa qué parte del texto es provisional y cuál ya quedó comprometida.

### LocalAgreement

LocalAgreement compara hipótesis producidas en actualizaciones consecutivas y confirma su
prefijo común más largo. Un ejemplo simplificado es:

```text
Hipótesis anterior: El presidente anunció una
Hipótesis nueva:    El presidente anunció medidas
Commit:             El presidente anunció
Provisional:        medidas
```

Con LocalAgreement-2, el commit depende de dos hipótesis consecutivas. Esta familia fue
formalizada para llevar modelos offline a escenarios simultáneos por Polák et al. (2022), y luego
aplicada a Whisper por Whisper-Streaming en 2023.

### AlignAtt

AlignAtt decide el punto de corte observando la atención cruzada entre los tokens generados y los
frames de audio. Si un token atiende principalmente los últimos frames disponibles, el sistema
asume que todavía puede depender de una palabra incompleta y detiene la emisión antes de ese
token.

La diferencia conceptual es:

```text
LocalAgreement: confirma porque varias hipótesis coincidieron.
AlignAtt:       emite porque la alineación del token está suficientemente lejos del borde.
```

AlignAtt puede controlar la latencia sin esperar necesariamente dos hipótesis coincidentes, pero
requiere acceso a información interna del decoder. Esa información no está expuesta de la misma
forma por CTranslate2, motivo por el cual la implementación oficial de SimulStreaming utiliza
Whisper sobre PyTorch.

### Simul-Whisper

Simul-Whisper aplicó AlignAtt a Whisper y agregó un detector de palabras truncadas en los límites
de chunks. El detector intenta identificar situaciones como `"el gobierno anun..."`, en las que
el modelo podría completar incorrectamente una palabra cuyo audio todavía no llegó.

Su contribución es principalmente algorítmica y experimental. La implementación original está
orientada a habla previamente segmentada por oración y a una simulación menos realista del costo
computacional.

### SimulStreaming

SimulStreaming es la continuación práctica de Whisper-Streaming y Simul-Whisper. Integra la
política AlignAtt con una interfaz para habla larga no segmentada, simulación desde archivo,
servidor TCP, VAC, contexto, prompts y beam search. Por ese motivo es el candidato principal para
nuestro experimento; no resulta necesario comenzar adaptando Simul-Whisper por separado.

## Evolución de la línea de investigación

| Año | Trabajo | Aporte relevante |
| ---: | --- | --- |
| 2022 | CUNI-KIT, Polák et al. | Formaliza la onlinización de modelos offline y LocalAgreement basado en el prefijo común de actualizaciones. |
| 2023 | Whisper-Streaming, Macháček et al. | Aplica LocalAgreement a Whisper para habla larga no segmentada y reporta una latencia media de 3,3 s en su evaluación. |
| 2023 | AlignAtt, Papi et al. | Propone utilizar alineaciones derivadas de atención para decidir cuándo emitir o esperar. El trabajo original evalúa traducción simultánea, no ASR español. |
| 2024 | Simul-Whisper, Wang et al. | Adapta AlignAtt a Whisper y agrega detección de palabras truncadas en bordes de chunk. |
| 2025 | SimulStreaming, Macháček y Polák | Extiende Simul-Whisper a audio continuo, `large-v3`, beam search, prompts, contexto y simulación computacionalmente consciente. |

Esta evolución no implica que cada trabajo invalide al anterior. LocalAgreement conserva una
ventaja importante: puede implementarse sobre distintos backends sin necesitar las matrices de
atención. AlignAtt ofrece una política más integrada con el decoder, a cambio de mayor
acoplamiento técnico.

## Pipeline actual

La ruta actualmente validada es:

```text
Placa
  -> PCM por TCP al bridge
  -> WebSocket hacia Colab
  -> faster-whisper / CTranslate2
  -> eventos partial/final
  -> filtro de acuerdo de prefijo
  -> bridge
  -> TCP de subtítulos
  -> SttAO / SubtitleAO
  -> overlay HDMI
```

### Componentes principales

| Componente | Implementación actual |
| --- | --- |
| Modelo | Whisper `small` |
| Backend | `faster-whisper` sobre CTranslate2 |
| Audio live | PCM de la placa, reenviado por el bridge |
| Segmentación | Silero VAD, silencio mínimo y límite máximo de ventana |
| Partials | Inferencia periódica sobre la frase acumulada |
| Estabilización | Prefijo común entre `N` hipótesis en `PartialStabilityFilter` |
| Finalización | Silencio, `max_window` o flush |
| Entrega | Protocolo TCP bidireccional con `session_ready` y `transcript_ack` |
| Validación | Audio capturado, JSONL, reconstrucción del overlay, métricas y ACK de placa |

El `partial_agreement=2` actual es una aproximación liviana a LocalAgreement-2, pero no es aún
un `HypothesisBuffer` completo. El filtro emite el prefijo estable como un nuevo partial, se
resetea con el final y no mantiene formalmente un historial persistente de texto comprometido,
timestamps y sufijo provisional.

### Configuración provisional de referencia

Los resultados actuales justifican conservar como baseline experimental:

| Parámetro | Valor |
| --- | ---: |
| modelo | `small` |
| `max_window_sec` | `3.0` |
| `min_silence_sec` | `0.5` |
| `partial_sec` | `1.0` |
| `partial_agreement` | `2` |
| VAD threshold / negative threshold | `0.50 / 0.35` |
| beam | `5` |
| gain | automático |

En la corrida `20260714-173719`, esta configuración obtuvo WER proxy `25,56%`, legibilidad
`66,67%`, p90 de recepción `1,09 s`, cero candidatos a alucinación y aceptación de los 87 eventos
por la placa. El WER es contra una pseudorreferencia offline y el p90 no es todavía latencia
palabra-a-pantalla; por lo tanto, estos valores son un baseline de ingeniería y no un resultado
final de tesis.

## Pipeline candidato con SimulStreaming

En una integración futura, la ruta sería:

```text
Placa
  -> PCM por TCP al bridge
  -> servidor SimulStreaming en Colab
  -> Whisper PyTorch + AlignAtt
  -> texto incremental y timestamps
  -> adaptador al protocolo de transcripts existente
  -> bridge y ACK de placa existentes
  -> overlay HDMI
```

La primera evaluación no requiere esta integración completa. SimulStreaming puede procesar los
mismos archivos de audio mediante su simulador oficial y producir JSONL incremental con tiempos
de emisión. Solamente si supera criterios previamente definidos se agregaría el adaptador live.

## Comparación técnica

| Dimensión | Pipeline actual | SimulStreaming + AlignAtt |
| --- | --- | --- |
| Política de emisión | Agreement simple entre hipótesis y finales por segmentación | Decodificación limitada por la alineación de atención |
| Backend | `faster-whisper/CTranslate2` | OpenAI Whisper adaptado sobre PyTorch |
| Acceso a atención | No requerido | Requerido |
| Costo incremental | Reprocesa la frase acumulada para cada partial | Conserva contexto y detiene el decoder según AlignAtt; igualmente procesa buffers con Whisper offline adaptado |
| Control de latencia | `partial_sec`, ventana, VAD y backpressure | chunk mínimo, `frame_threshold`, buffer, VAC y costo real del decoder |
| Palabras cortadas | VAD y corrección posterior en futuras hipótesis | AlignAtt más detector CIF/TDM opcional |
| Contexto entre buffers | Limitado; no hay prompt desde confirmados | Soportado por la implementación oficial |
| Modelos | Modelos convertidos para faster-whisper | Modelos `.pt` compatibles con la adaptación PyTorch |
| Memoria y velocidad | Ya medido con `small` en Colab | Debe medirse; la recomendación oficial de 10 GB aplica a `large-v3`, aunque admite modelos menores |
| Integración actual | Completa, incluida placa y reportes | Inexistente; primero se evalúa standalone |
| Madurez en nuestro entorno | Alta | Desconocida |
| Valor académico | Baseline propio instrumentado | Comparación con una política publicada basada en atención |

## Evidencia publicada y alcance real

### Evidencia favorable

El paper de Simul-Whisper reporta, sobre múltiples datasets e idiomas, una degradación absoluta
media de WER de `1,46` puntos al usar chunks de un segundo respecto al procesamiento offline.
También reporta una mejor relación WER-latencia que su baseline de LocalAgreement. El método no
requiere ajustar los pesos principales de Whisper, aunque el detector de truncación sí introduce
un módulo adicional.

El paper original de AlignAtt reporta mejoras de calidad y reducciones de latencia frente a otras
políticas simultáneas en ocho pares de idiomas de MuST-C. Este resultado sustenta la política de
atención, pero pertenece a traducción simultánea y no constituye evidencia directa de
transcripción española.

SimulStreaming fue utilizado en la participación CUNI de IWSLT 2025 e integra AlignAtt con
Whisper, beam search, prompts y contexto para habla continua. Su repositorio ofrece tanto
simulación computacionalmente consciente como una variante que ignora el costo de inferencia.

### Lo que los papers no demuestran para este proyecto

La evidencia disponible no demuestra todavía:

- que AlignAtt supere a nuestro pipeline `faster-whisper` en español;
- que una GPU gratuita de Colab mantenga tiempo real sin acumular cola;
- que el resultado cumpla p90 de palabra a primera aparición menor o igual a `1,5 s`;
- que el comportamiento sea estable en audio de televisión, noticias y ruido continuo;
- que la mayor complejidad compense la mejora obtenida;
- ni que la afirmación de aproximadamente cinco veces más velocidad frente a
  Whisper-Streaming se traslade a nuestro servidor propio basado en CTranslate2.

Estas preguntas abiertas no invalidan los trabajos. Definen el experimento que debe realizarse
para aplicar sus conclusiones a este sistema.

## Hipótesis de investigación

### Hipótesis principal

> Para un mismo tamaño de Whisper y chunks cercanos a un segundo, AlignAtt puede reducir el
> retraso necesario para obtener texto estable y disminuir las revisiones provocadas por palabras
> incompletas, sin degradar significativamente el WER final frente al pipeline incremental actual.

### Hipótesis secundarias

1. El beneficio será mayor en habla continua que en clips con pausas claras, porque AlignAtt no
   depende exclusivamente del endpointing por silencio.
2. El backend PyTorch consumirá más memoria o tiempo de cómputo que CTranslate2, por lo que la
   mejora algorítmica puede no traducirse directamente en menor latencia computacional.
3. El detector CIF/TDM reducirá errores de borde, pero su efecto debe medirse por separado de
   AlignAtt.
4. `medium` podría tolerar mejor el streaming que `small`, pero no debe cambiarse modelo y
   política en el mismo primer experimento.

## Plan experimental propuesto

### Fase 0: preparar referencias y congelar el baseline

1. Crear `desay-short.txt`, `noticiero-short.txt` y `rel-short.txt` con transcripción humana.
2. Conservar la configuración actual `small / 3.0 / 0.5 / 1.0 / agreement=2` como baseline.
3. Guardar versión de código, configuración efectiva, GPU de Colab y modelo utilizado.
4. No modificar todavía firmware, protocolo TCP ni overlay.

### Fase 1: SimulStreaming standalone en Colab

Procesar los tres audios mediante el simulador oficial:

| Parámetro | Valor inicial |
| --- | --- |
| tarea | transcripción |
| idioma | español explícito |
| modelo | Whisper `small` sobre PyTorch |
| chunk mínimo | `1,0 s` |
| VAC | activado |
| beam | `1` |
| evaluación temporal | computationally aware |
| salida | JSONL incremental con timestamps y emission time |

El directorio del modelo `faster-whisper-small` utilizado actualmente no se puede reutilizar de
forma directa porque contiene una conversión para CTranslate2. La prueba requiere el checkpoint
`.pt` compatible con OpenAI Whisper/SimulStreaming. Si la descarga desde Colab vuelve a ser un
problema, el checkpoint puede prepararse localmente y subirse de la misma manera que se hizo con
el modelo faster-whisper.

### Fase 2: sensibilidad de AlignAtt

El paper de Simul-Whisper utiliza un margen de `12` frames, equivalente a aproximadamente
`240 ms` en su configuración. Se propone un barrido acotado:

| Caso | `frame_threshold` | Intención |
| --- | ---: | --- |
| agresivo | `8` | emitir más cerca del borde y priorizar latencia |
| referencia | `12` | reproducir el punto documentado por Simul-Whisper |
| conservador | `16` | esperar más contexto y priorizar estabilidad |

Antes de fijar estos valores en el notebook se verificará la unidad efectiva para el modelo y la
versión exacta de SimulStreaming utilizada.

### Fase 3: truncation detection

Sólo después de medir AlignAtt sin módulos adicionales:

1. seleccionar un checkpoint CIF/TDM compatible con `small`;
2. repetir el mejor `frame_threshold` con el detector activado;
3. contar específicamente correcciones de palabras partidas en límites de chunk;
4. medir el costo adicional de cómputo y memoria.

No se combinará inicialmente esta prueba con `large-v3`, ya que el repositorio oficial advierte
que no dispone de un checkpoint CIF para ese modelo.

### Fase 4: integración live condicional

Si el standalone resulta prometedor, implementar un adaptador de producción:

- entrada PCM desde el bridge;
- salida al esquema actual de transcripts;
- secuencia por sesión;
- mismos `session_ready` y `transcript_ack` de la placa;
- mismos artefactos de trazabilidad y análisis;
- sin rutas ni estados exclusivos de testing en firmware.

El backend actual permanecerá seleccionable para poder hacer pruebas A/B sobre el mismo audio y
recuperar el sistema inmediatamente ante cualquier regresión.

## Métricas para la comparación

### Calidad

- WER y CER finales contra referencia humana;
- degradación del modo incremental frente al mismo modelo offline;
- errores de palabras truncadas;
- alucinaciones y repeticiones.

### Latencia

- tiempo hasta el primer subtítulo;
- palabra pronunciada a primera aparición;
- palabra pronunciada a confirmación estable;
- final de habla a finalización del segmento;
- tiempo de inferencia p50, p90, p95 y máximo;
- real-time factor y profundidad máxima de cola.

La simulación `computationally unaware` puede utilizarse como límite algorítmico inferior, pero la
decisión de producto debe basarse en la simulación `computationally aware` y en el test live.

### Estabilidad y display

- cantidad de revisiones por palabra;
- longitud del prefijo estable;
- frecuencia de actualización;
- duración visible de cada estado;
- cambios sobre texto previamente mostrado;
- reconstrucción lógica del overlay;
- observación física HDMI como validación manual separada.

### Confiabilidad

- chunks de audio perdidos;
- partials omitidos por backpressure;
- finales perdidos;
- eventos generados, enviados y aceptados;
- ACK ausentes o rechazados;
- reconexiones y errores de protocolo.

## Criterios de decisión

SimulStreaming se considerará candidato de integración estable si, sobre referencias humanas y
con tres réplicas de los casos principales:

1. reduce de forma consistente la latencia a texto estable o el revision rate;
2. no empeora materialmente el WER final;
3. procesa audio en tiempo real sin crecimiento sostenido de cola;
4. cabe de forma reproducible en la GPU asignada por Colab;
5. conserva 100% de finales y 100% de ACK aceptados en el test live;
6. y produce una mejora visible de lectura que justifique el aumento de complejidad.

No se utilizará un puntaje global único. Exactitud, latencia, estabilidad, recursos y
confiabilidad se presentarán por separado.

## Posible aporte a la memoria de tesis

Una formulación inicial para la memoria podría ser:

> Aunque Whisper fue diseñado para transcripción offline, puede utilizarse en escenarios de baja
> latencia mediante políticas de inferencia incremental. Whisper-Streaming aplica LocalAgreement,
> confirmando el prefijo común de hipótesis sucesivas, mientras que Simul-Whisper y
> SimulStreaming emplean AlignAtt para detener la decodificación cuando los tokens se alinean con
> la región más reciente e incierta del audio. En este trabajo se compara una implementación
> instrumentada basada en faster-whisper y acuerdo de prefijos con SimulStreaming sobre el mismo
> corpus español, distinguiendo calidad final, latencia computacional, latencia perceptual,
> estabilidad visual y entrega al sistema embebido.

El valor del experimento no dependería de que AlignAtt resulte ganador. Si no supera al pipeline
actual, el resultado igualmente permitiría documentar el costo de trasladar una política basada
en atención a un entorno real con GPU compartida, red, firmware y overlay físico. Si lo supera,
ofrecería una justificación bibliográfica y experimental para adoptar una arquitectura más
cercana al estado del arte publicado.

## Bibliografía y repositorios primarios

- Polák, P., Pham, N.-Q., Nguyen, T. N., et al. (2022). *CUNI-KIT System for
  Simultaneous Speech Translation Task at IWSLT 2022*. IWSLT 2022, pp. 277-285.
  [ACL Anthology](https://aclanthology.org/2022.iwslt-1.24/),
  [DOI](https://doi.org/10.18653/v1/2022.iwslt-1.24).
- Macháček, D., Dabre, R. y Bojar, O. (2023). *Turning Whisper into Real-Time
  Transcription System*. IJCNLP-AACL 2023 System Demonstrations, pp. 17-24.
  [ACL Anthology](https://aclanthology.org/2023.ijcnlp-demo.3/),
  [DOI](https://doi.org/10.18653/v1/2023.ijcnlp-demo.3),
  [repositorio Whisper-Streaming](https://github.com/ufal/whisper_streaming).
- Papi, S., Turchi, M. y Negri, M. (2023). *AlignAtt: Using Attention-based
  Audio-Translation Alignments as a Guide for Simultaneous Speech Translation*.
  Interspeech 2023, pp. 3974-3978.
  [ISCA Archive](https://www.isca-archive.org/interspeech_2023/papi23_interspeech.html),
  [DOI](https://doi.org/10.21437/Interspeech.2023-170).
- Wang, H., Hu, G., Lin, G., Zhang, W.-Q. y Li, J. (2024). *Simul-Whisper:
  Attention-Guided Streaming Whisper with Truncation Detection*.
  [arXiv:2406.10052](https://arxiv.org/abs/2406.10052),
  [repositorio](https://github.com/backspacetg/simul_whisper).
- Macháček, D. y Polák, P. (2025). *Simultaneous Translation with Offline Speech
  and LLM Models in CUNI Submission to IWSLT 2025*. IWSLT 2025, pp. 389-398.
  [ACL Anthology](https://aclanthology.org/2025.iwslt-1.41/),
  [DOI](https://doi.org/10.18653/v1/2025.iwslt-1.41),
  [repositorio SimulStreaming](https://github.com/ufal/SimulStreaming).

## Fuentes internas relacionadas

- `docs/streaming_stt_run_log.md`: evolución y resultados de corridas live.
- `docs/streaming_stt_setup.md`: arquitectura actual del servidor streaming y bridge.
- `logs/audio-tests/sweeps/20260714-172152/report.md`: factorial más reciente con ACK de placa.
- `deep-research-report (1).md`: informe de investigación que motivó esta comparación; sus
  identificadores internos de citas deben sustituirse por las referencias primarias anteriores
  antes de reutilizar texto en la memoria.
