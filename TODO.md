# TODO — Plataforma de Visao Computacional

Baseado no documento de Arquitetura e Auditoria v1.0.
Sequencia de implementacao segue a recomendacao da secao 21: pipeline unico primeiro,
depois encapsulamento, persistencia, modularizacao e por fim arquitetura distribuida.

---

## Fase 1 — MVP Tecnico Fim a Fim

### A1: Decoder e Sampler

- [ ] Criar estrutura de diretorios do projeto (`core/`, `cmd/`, `configs/`, `models/`, `scripts/`)
- [ ] Configurar build system (Makefile/CMake) para compilacao em C
- [ ] Integrar FFmpeg como dependencia (linking, headers, flags)
- [ ] Implementar modulo de abertura e leitura de video (`video_decoder.c`)
  - [ ] Abrir container de video via `avformat`
  - [ ] Localizar stream de video e inicializar codec via `avcodec`
  - [ ] Decodificar frames em loop e converter para formato intermediario (RGB24/BGR24)
  - [ ] Tratar erros de codec, container corrompido e EOF
- [ ] Implementar frame sampler (`frame_sampler.c`)
  - [ ] Modo completo: todos os frames
  - [ ] Modo intervalo fixo: 1 a cada N frames
  - [ ] Interface para politica de sampling configuravel
- [ ] Teste: decodificar video piloto e salvar frames como imagem para validacao visual

### A2: Engine de Inferencia Plugavel

- [ ] Definir struct `InferenceBackendOps` com interface uniforme (`init`, `infer`, `get_output_shape`, `destroy`)
- [ ] Definir struct `InferenceBackend` com ponteiro para ops e estado interno
- [ ] Implementar backend ONNX Runtime como primeiro plugin (`backend_onnx.c`)
  - [ ] `init`: carregar modelo `.onnx`, criar sessao, configurar device (CPU)
  - [ ] `infer`: alimentar tensor de entrada, executar sessao, copiar saida
  - [ ] `get_output_shape`: retornar dimensoes do tensor de saida
  - [ ] `destroy`: liberar sessao, allocator e recursos
- [ ] Criar mecanismo de selecao de backend em runtime (factory por string/enum)
- [ ] Teste: carregar modelo YOLO exportado em ONNX e executar inferencia em tensor dummy

### Preprocessamento

- [ ] Implementar modulo de preprocessamento (`preprocessor.c`)
  - [ ] Resize/letterbox para dimensao esperada pelo modelo (ex: 640x640)
  - [ ] Normalizacao de pixels (0-255 -> 0.0-1.0 ou conforme contrato do modelo)
  - [ ] Reordenacao de canais (HWC -> CHW) se necessario
  - [ ] Escrita em buffer de tensor contiguo e alinhado
- [ ] Garantir que preprocessamento e deterministico e documentar parametros

### Pos-processamento Basico

- [ ] Implementar modulo de pos-processamento (`postprocessor.c`)
  - [ ] Decodificar bounding boxes da saida bruta (formato YOLO)
  - [ ] Filtrar por threshold de confianca
  - [ ] Implementar NMS (Non-Maximum Suppression) com threshold de IoU
  - [ ] Mapear class IDs para nomes via arquivo de labels
- [ ] Teste: validar deteccoes em frames conhecidos contra ground truth manual

### Catalogacao e Exportacao Simples

- [ ] Implementar struct de deteccao (`detection.h`): frame_number, timestamp_ms, class_id, confidence, bbox
- [ ] Implementar acumulador de catalogo basico por classe (contagem, first_seen, last_seen)
- [ ] Exportar resultados em CSV (deteccoes por frame)
- [ ] Exportar resultados em JSON (catalogo resumido)
- [ ] Teste: processar video piloto fim a fim e validar CSV/JSON gerados

### Pipeline Unico Integrado

- [ ] Integrar todos os modulos no fluxo: decode -> sample -> preprocess -> infer -> postprocess -> catalog
- [ ] Criar `main.c` em `cmd/worker/` que executa o pipeline completo para um video
- [ ] Aceitar argumentos de CLI: video path, modelo, threshold, sampling rate, output path
- [ ] Medir e imprimir tempo total e FPS efetivo
- [ ] Teste fim a fim: video piloto -> deteccoes CSV + catalogo JSON

---

## Fase 2 — Worker Modular

### Modularizacao do Pipeline

- [ ] Refatorar pipeline em estagios com interface clara (entrada/saida tipada por estagio)
- [ ] Separar estado do job em struct `JobContext` (paths, parametros, buffers, metricas)
- [ ] Implementar `job_loader.c`: carrega contexto do job a partir de config/args
- [ ] Isolar `metrics_collector.c`: medir latencia por estagio (decode, preprocess, infer, postprocess)
- [ ] Isolar `result_publisher.c`: persistencia dos resultados finais

### Gerenciamento de Memoria

- [ ] Implementar pool de buffers reutilizaveis para frames decodificados
- [ ] Implementar pool de buffers para tensores de entrada/saida
- [ ] Implementar pool para structs de deteccao
- [ ] Eliminar malloc/free por frame no hot path
- [ ] Definir limites de memoria por pool e validar em runtime

### Tracking Basico

- [ ] Implementar tracker simples por IoU entre frames consecutivos (`tracker.c`)
  - [ ] Associar deteccoes atuais com tracks anteriores por IoU
  - [ ] Atribuir track_id a deteccoes associadas
  - [ ] Criar novos tracks para deteccoes nao associadas
  - [ ] Encerrar tracks inativos apos N frames sem match
- [ ] Atualizar catalogo para contar por track (objetos unicos) alem de por deteccao
- [ ] Teste: verificar que catalogo nao infla contagem por repeticao frame a frame

### Metricas por Estagio

- [ ] Coletar latencia media, p95, p99 por estagio do pipeline
- [ ] Coletar FPS efetivo, pico de memoria, taxa de erro
- [ ] Implementar struct `ProcessingMetrics` com todos os campos
- [ ] Imprimir relatorio de metricas ao final do job
- [ ] Exportar metricas em JSON junto com resultados

---

## Fase 3 — Arquitetura Distribuida

### Modelo de Dados e Persistencia (PostgreSQL)

- [ ] Definir schema SQL para tabelas:
  - [ ] `video_job` (job_id, source_uri, status, priority, profile_id, engine_id, model_version_id)
  - [ ] `processing_profile` (profile_id, name, sampling_policy, input_resolution, thresholds, batch_size)
  - [ ] `inference_engine` (engine_id, name, version, backend_type, device_type, precision_mode)
  - [ ] `runtime_environment` (runtime_env_id, cpu_model, gpu_model, memory_gb, os_version, driver_version)
  - [ ] `frame_detection` (job_id, frame_number, timestamp_ms, class_id, confidence, bbox, track_id)
  - [ ] `catalog_summary` (job_id, class_id, total_detections, total_unique_tracks, first_seen_ms, last_seen_ms)
  - [ ] `processing_metrics` (job_id, decode_avg_ms, infer_avg_ms, p95, p99, effective_fps, peak_memory_mb)
- [ ] Implementar migrations e scripts de setup do banco
- [ ] Implementar camada de acesso a dados no worker (libpq ou wrapper)

### Servico de Ingestao

- [ ] Criar servico HTTP/gRPC basico para receber submissoes de video (`cmd/ingestion_service/`)
- [ ] Validar metadados minimos: tamanho, codec, duracao, integridade
- [ ] Registrar job com status RECEIVED no banco
- [ ] Resolver perfil operacional padrao quando nao informado
- [ ] Calcular assinatura (hash) da configuracao submetida
- [ ] Publicar job na fila

### Broker de Jobs

- [ ] Escolher e integrar message broker (ex: RabbitMQ, Redis Streams, ou NATS)
- [ ] Implementar publicacao de jobs pela ingestao
- [ ] Implementar consumo de jobs pelo worker
- [ ] Configurar dead-letter queue para jobs que excedam max retries
- [ ] Implementar backpressure basico

### Scheduler / Dispatcher

- [ ] Implementar logica de despacho por perfil operacional
- [ ] Suportar pools de execucao (low_latency, throughput, etc.)
- [ ] Despachar por compatibilidade de engine/hardware
- [ ] Suportar prioridade de jobs

### Estados do Job

- [ ] Implementar maquina de estados: RECEIVED -> QUEUED -> RUNNING -> COMPLETED/FAILED/PARTIAL
- [ ] Suportar estados RETRYING e CANCELLED
- [ ] Registrar transicoes de estado com timestamp
- [ ] Distinguir erro de video corrompido vs erro interno de worker
- [ ] Implementar retry com limite configuravel

### Multiplos Workers

- [ ] Adaptar worker para consumir jobs da fila em loop
- [ ] Garantir que workers sao independentes e stateless entre jobs
- [ ] Testar execucao com 2+ workers simultaneos
- [ ] Validar isolamento de falhas entre workers

---

## Fase 4 — Auditoria Comparativa

### Trilha Auditavel

- [ ] Definir schema para tabelas de auditoria:
  - [ ] `audit_run` (audit_run_id, job_id, profile_id, engine_id, model_version_id, runtime_env_id, started_at, finished_at, status)
  - [ ] `audit_parameter_snapshot` (audit_run_id, parameter_key, parameter_value)
  - [ ] `audit_stage_metrics` (audit_run_id, stage, avg_ms, p95_ms, p99_ms, peak_memory_mb, error_count)
  - [ ] `audit_comparison` (comparison_id, comparison_type, baseline_run_id, candidate_run_ids, result_json)
- [ ] Implementar congelamento de parametros efetivos no inicio de cada execucao
- [ ] Registrar assinatura de video/dataset, versao do modelo, class mapping, engine, host e driver
- [ ] Implementar `audit_run` lifecycle: criar -> coletar -> finalizar

### Metricas Primarias e Secundarias

- [ ] Coletar indicadores primarios: latencia media, p95, p99, FPS, throughput, uso CPU/GPU/memoria, taxa de erro
- [ ] Calcular indicadores secundarios:
  - [ ] Estabilidade de latencia (desvio padrao entre repeticoes)
  - [ ] Eficiencia de memoria (frames por MB)
  - [ ] Robustez (taxa de erro + completude)
  - [ ] Consistencia de catalogo (similaridade entre repeticoes)
  - [ ] Eficiencia operacional (objetos/segundo)

### Perfis Operacionais

- [ ] Implementar struct `ProcessingProfile` com todos os campos (resolucao, sampling, thresholds, batch_size, engine, tracking, timeout, retry, limites de memoria)
- [ ] Criar perfis pre-definidos: Low Latency, High Throughput, Cost Efficient, Balanced
- [ ] Permitir selecao de perfil por job
- [ ] Persistir perfil efetivo junto ao audit_run

### Comparacao entre Execucoes

- [ ] Implementar validacao de justica experimental (mesmas variaveis controladas)
- [ ] Implementar normalizacao de metricas (menor-melhor vs maior-melhor)
- [ ] Implementar score composto com pesos configuraveis por cenario
- [ ] Implementar calculo de deltas percentuais contra baseline
- [ ] Gerar relatorio comparativo (JSON e/ou texto)

### Matriz de Experimentos

- [ ] Implementar suporte para experimentos catalogados (E01-E04)
- [ ] E01: comparar perfis operacionais (video, modelo, engine, hardware fixos)
- [ ] E02: comparar engines de inferencia (video, modelo, perfil, hardware fixos)
- [ ] E03: comparar versoes de modelo (video, engine, perfil, hardware fixos)
- [ ] E04: comparar ambientes (video, engine, perfil, modelo fixos)
- [ ] Suportar N repeticoes por experimento (minimo 10)
- [ ] Reportar media, dispersao e percentis por experimento

---

## Fase 5 — Otimizacao e Robustez

### Backends Adicionais

- [ ] Implementar backend TensorRT (`backend_tensorrt.c`)
- [ ] Implementar backend OpenVINO (`backend_openvino.c`)
- [ ] Validar que troca de backend nao altera pipeline de dominio
- [ ] Benchmark comparativo entre os 3 backends

### Paralelismo Interno do Worker

- [ ] Nivel 2: desacoplar estagios por filas internas com threads dedicadas (decode | preprocess | infer | postprocess)
- [ ] Nivel 3: habilitar paralelismo interno da engine (multi-thread ONNX, CUDA streams TensorRT)
- [ ] Profiling para validar que paralelismo nao mascara gargalos

### Observabilidade

- [ ] Implementar logs estruturados com job_id, audit_run_id, stage, worker_id, timestamp
- [ ] Padronizar formato de log entre ingestao, dispatcher e worker
- [ ] Implementar tracing logico: ingestion_request_id -> job_id -> audit_run_id -> worker_id -> export_id
- [ ] Dashboard de metricas operacionais (fila, jobs ativos, FPS por perfil/engine)

### Tolerancia a Falhas Avancada

- [ ] Retry com backoff exponencial e limite
- [ ] Dead-letter queue funcional com alertas
- [ ] Timeout por estagio com registro de contexto
- [ ] Protecao contra perda de estado parcial em falha de storage

### Seguranca e Governanca

- [ ] Separar credenciais por ambiente (dev/staging/prod)
- [ ] Versionamento de modelos e class mappings
- [ ] Assinatura hash da configuracao efetiva por audit_run
- [ ] Politica de retencao para videos, frames, exports e logs
- [ ] Prever anonimizacao/mascaramento para videos com dados sensiveis

### Sampling Adaptativo

- [ ] Implementar sampling adaptativo (baseado em movimento ou mudanca de cena)
- [ ] Integrar sampling adaptativo como opcao no perfil operacional

### Autoscaling

- [ ] Mecanismo de adicao/remocao de workers baseado em profundidade da fila
- [ ] Politicas de scale-up e scale-down por pool

---

## Criterios de Sucesso (Validacao Final)

- [ ] Pipeline em C processa videos fim a fim com deteccao e catalogacao consistentes
- [ ] Arquitetura suporta distribuicao por multiplos workers
- [ ] Cada execucao produz metricas por estagio e trilha auditavel para replay analitico
- [ ] Engine de inferencia pode ser trocada sem alterar pipeline de dominio
- [ ] Perfis operacionais podem ser comparados de forma controlada
- [ ] Sistema responde com dados qual engine/perfil entrega melhor resultado por cenario
