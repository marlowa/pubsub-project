// Example: logon, send 5 limit buy orders, logout.
//
// fix.uniqueId() produces a unique ClOrdID each call, so this script can be
// run multiple times in the same session without generating duplicate ClOrdIDs.
//
// The gateway FIX listener (port 9879) requires TLS — useTls must be true.

session.logon("APM001", "stubpassword", true)

// Wait up to 5 seconds for the logon to complete.
def deadline = System.currentTimeMillis() + 5000
while (!session.isLoggedOn() && System.currentTimeMillis() < deadline) {
    sleep(200)
}
if (!session.isLoggedOn()) {
    out.println "ERROR: logon did not complete within 5 seconds"
    return
}
out.println "Logged on"

5.times {
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID(fix.uniqueId()))
    nos.set(new quickfix.field.Symbol("BHP"))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(100))
    nos.set(new quickfix.field.Price(10.50))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
    sleep(200)
}
out.println "Sent 5 orders"

sleep(3000)
session.logout()
out.println "Done"
