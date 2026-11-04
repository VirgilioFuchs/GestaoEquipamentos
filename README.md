# GestaoEquipamentos

![Licença: MIT](https://img.shields.io/badge/Licen%C3%A7a-MIT-green.svg)

Aplicação desktop para gestão local de equipamentos (notebooks), com controle de cadastro, movimentações, pessoas, locais, contas de acesso e trilha de auditoria.

## O que o projeto faz

O sistema registra todo o ciclo operacional dos equipamentos:

- Cadastro e edição de equipamentos com patrimônio único.
- Controle de status: disponível, emprestado e manutenção.
- Registro de retirada, devolução e atualização de local.
- Cadastro de pessoas responsáveis e locais operacionais.
- Controle de contas (principal, administradores e comuns) com autenticação.
- Histórico de eventos com filtros por período, equipamento, local, pessoa, conta e tipo.

## Funcionalidades principais

- **Inventário operacional** com ações por status do equipamento.
- **Movimentações rastreáveis** (quem retirou/devolveu, quando e onde).
- **Sessão administrativa com expiração** e reautenticação.
- **Recuperação de conta principal** por autenticador/códigos de recuperação.
- **Persistência local em SQLite**, com migrações de esquema e logs append-only.

## Stack e tecnologias

- **Linguagem:** C++17
- **UI desktop:** Qt 6 (Widgets)
- **Banco local:** SQLite (via Qt SQL)
- **Build:** CMake + Ninja
- **Criptografia/segurança:** libsodium + QtKeychain
- **Testes:** Qt Test (opcional via BUILD_TESTING)

## Estrutura de pastas

```text
GestaoEquipamentos/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
├── resources/
├── src/
│   ├── admin/
│   ├── application/
│   ├── domain/
│   ├── security/
│   ├── storage/
│   └── ui/
├── third_party/
├── tools/
└── docs/
    └── PROCESSO.md
```

## Como compilar e executar

### Pré-requisitos

- Qt 6.4+ (Widgets e SQL com driver QSQLITE)
- CMake 3.21+
- Ninja
- Compilador C++17
- libsodium e QtKeychain (ou resolução automática conforme plataforma/configuração)

### Build (preset de desenvolvimento)

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/GestaoEquipamentos
```

### Sem testes

```bash
cmake --preset dev -DBUILD_TESTING=OFF
cmake --build --preset dev
./build/dev/GestaoEquipamentos
```

## Como funciona

```mermaid
flowchart TD
    A[Login / Reautenticação] --> B[Autorização por perfil]
    B --> C[Operações de catálogo<br/>Pessoas, Locais, Equipamentos]
    B --> D[Movimentações<br/>Retirada, Devolução, Manutenção, Local]
    C --> E[(SQLite Local)]
    D --> E
    E --> F[Eventos append-only]
    F --> G[Página de Logs com filtros]
```

## Fluxo principal em alto nível

1. Usuário autentica e abre sessão.
2. O sistema autoriza a operação conforme perfil da conta.
3. A ação é gravada no banco local (inventário/catálogos/empréstimos).
4. Um evento de histórico é anexado com metadados da operação.
5. A interface reflete o novo estado e os logs podem ser consultados com filtros.

## Licença

Este projeto está sob a licença **MIT**. Consulte o arquivo [LICENSE](LICENSE).
