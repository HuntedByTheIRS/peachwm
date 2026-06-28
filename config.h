static void dwindle(Monitor *m);

static const Layout layouts[] = {
    {"><>", NULL},
    {"[T]", dwindle},
};
