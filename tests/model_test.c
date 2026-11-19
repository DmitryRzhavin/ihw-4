#include "model.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
static void prepare(Model *m, int visitors, int lifo)
{
    Config c;
    memset(&c, 0, sizeof(c));
    c.visitor_count = visitors; c.waiter_count = 2; c.cook_count = 2; c.menu_count = 1; c.unit_ms = 1;
    c.kitchen_cap = 2; c.ready_cap = 1; c.lifo = lifo; c.equipment[0] = 1;
    strcpy(c.menu[0].name, "Soup"); c.menu[0].count = 2;
    c.menu[0].steps[0] = (OperationSpec){0, 1}; c.menu[0].steps[1] = (OperationSpec){0, 1};
    for (int i = 0; i < visitors; ++i) c.dish_count[i] = 1;
    model_init(m, &c);
    for (int i = 0; i < visitors; ++i) {
        assert(model_apply(m, (Request){HELLO, VISITOR, i, 0}).code == 0);
        model_apply(m, (Request){ARRIVE, VISITOR, i, 0});
    }
    for (int i = 0; i < 2; ++i) {
        model_apply(m, (Request){HELLO, WAITER, i, 0});
        model_apply(m, (Request){HELLO, COOK, i, 0});
    }
    for (int i = 0; i < visitors; ++i) model_apply(m, (Request){WAITER_STEP, WAITER, i % 2, 0});
    model_tick(m);
    assert(model_check(m) == 0);
}
int main(void)
{
    Model m;
    for (int lifo = 0; lifo < 2; ++lifo) {
        prepare(&m, 2, lifo);
        Reply r = model_apply(&m, (Request){COOK_TAKE, COOK, 0, 0});
        assert(r.code == 1 && m.jobs[r.value].order == (lifo ? 1 : 0));
        assert(model_apply(&m, (Request){COOK_TAKE, COOK, 0, 0}).code == -2);
        assert(model_apply(&m, (Request){COOK_TAKE, COOK, 1, 0}).code == 0);
        assert(model_apply(&m, (Request){COOK_FINISH, COOK, 1, r.value}).code == -2);
        assert(model_apply(&m, (Request){COOK_FINISH, COOK, 0, r.value}).code == 1);
        assert(m.jobs[r.value].step == 1 && m.jobs[r.value].state == 1);
        assert(model_check(&m) == 0);
        ++m.queued; assert(model_check(&m) == -1);
    }
    /* Повторное подключение занятого номера отвергается, после отключения разрешено. */
    prepare(&m, 2, 0);
    assert(model_apply(&m, (Request){HELLO, COOK, 0, 0}).code == -2);
    Reply r = model_apply(&m, (Request){COOK_TAKE, COOK, 0, 0});
    assert(r.code == 1 && m.equipment_owner[0][0] == 0);
    model_drop(&m, COOK, 0);
    assert(m.aborted == 1 && m.equipment_owner[0][0] == -1 && m.cook_job[0] == -1);
    assert(m.jobs[r.value].state == 1 && model_check(&m) == 0);
    assert(model_apply(&m, (Request){COOK_TAKE, COOK, 0, 0}).code == -2);
    assert(model_apply(&m, (Request){HELLO, COOK, 0, 0}).code == 0 && m.reconnects == 1);
    model_tick(&m);
    assert(model_apply(&m, (Request){COOK_TAKE, COOK, 0, 0}).code == 1);
    /* Отключённый официант оставляет заказ, другой официант подхватывает его. */
    prepare(&m, 1, 0);
    assert(m.orders[0].waiter == 0);
    model_drop(&m, WAITER, 0);
    assert(m.orders[0].waiter == -1);
    Reply a = model_apply(&m, (Request){WAITER_STEP, WAITER, 1, 0});
    assert(a.value == 3 && m.orders[0].waiter == 1);
    /* Заказ не выдаётся, пока посетитель отключён. */
    prepare(&m, 1, 0);
    for (int step = 0; step < 2; ++step) {
        Reply t = model_apply(&m, (Request){COOK_TAKE, COOK, 0, 0});
        assert(t.code == 1);
        assert(model_apply(&m, (Request){COOK_FINISH, COOK, 0, t.value}).code == 1);
        model_tick(&m);
    }
    model_tick(&m);
    assert(m.orders[0].state == 4);
    model_drop(&m, VISITOR, 0);
    model_apply(&m, (Request){WAITER_STEP, WAITER, 0, 0});
    assert(m.delivered == 0 && m.orders[0].state == 4);
    assert(model_apply(&m, (Request){HELLO, VISITOR, 0, 0}).value == 1);
    model_apply(&m, (Request){WAITER_STEP, WAITER, 0, 0});
    assert(m.delivered == 1 && model_check(&m) == 0);
    return 0;
}
