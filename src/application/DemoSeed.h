#pragma once
#include "application/AccountService.h"
#include "application/AuthService.h"
#include "application/LocationService.h"
#include "application/PeopleService.h"
#include "storage/NotebookStore.h"

// Somente DataProfile::Demo. Cria demo.principal se não houver principal, cadastra locais quando o catálogo está vazio,
// insere os notebooks fictícios por id de local apenas em inventário vazio (um disponível, dois emprestados e um em
// manutenção, além de uma devolução por outra pessoa), cria a conta comum demo.operador uma única vez, registra uma
// retirada de NB-001 por Ana Souza se ele nunca foi emprestado, acrescenta uma única vez NB-005 (retirada e devolução
// por demo.operador, depois inativado), uma homônima Ana Souza auxiliar, uma pessoa inativa e o local excluído Sala 7,
// e devolve a sessão de demo.principal.
Session seedDemo(AuthService &auth, NotebookStore &store, AccountService &accounts, DataProfile profile,
                 LocationService &locations, PeopleService &people);
// Expande o perfil demo até 25 professores, 25 auxiliares, 50 locais e 50 equipamentos.
Session seedDemoDataset150(AuthService &auth, NotebookStore &store, AccountService &accounts, DataProfile profile,
                           LocationService &locations, PeopleService &people);
