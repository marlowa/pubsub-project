// Example: logon, send 5 orders, logout.
session.logon()
sleep(2000)

5.times { i ->
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID("ORD-00${i + 1}"))
    nos.set(new quickfix.field.Symbol("BHP"))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(100))
    nos.set(new quickfix.field.Price(10.50))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
    sleep(200)
}

sleep(3000)
session.logout()
