#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <lardon3d/project.h>

bool
lardon3d_project_set_name(Lardon3DAppState *state, const char *name)
{
    if (!state) {
        return false;
    }

    if (!name) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Erreur : le nom du projet est vide."
        );
        return false;
    }

    const char *start = name;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }

    const char *end = name + strlen(name);
    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }

    size_t length = (size_t)(end - start);
    if (length == 0) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Erreur : le nom du projet est vide."
        );
        return false;
    }

    if (length >= sizeof(state->project_name)) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Erreur : le nom du projet est trop long."
        );
        return false;
    }

    (void)memcpy(state->project_name, start, length);
    state->project_name[length] = '\0';
    state->project_loaded = true;
    (void)snprintf(
        state->status_message,
        sizeof(state->status_message),
        "Projet créé : %s",
        state->project_name
    );
    return true;
}

void
lardon3d_project_close(Lardon3DAppState *state)
{
    if (!state) {
        return;
    }

    if (!state->project_loaded) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Aucun projet à fermer."
        );
        return;
    }

    state->project_loaded = false;
    state->project_name[0] = '\0';
    (void)snprintf(
        state->status_message,
        sizeof(state->status_message),
        "Projet fermé."
    );
}
