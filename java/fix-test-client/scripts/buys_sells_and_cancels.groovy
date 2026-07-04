// Place 1000 buy orders and 1000 sell orders, then cancel the first 250 of each.
//
// Phase 1 -- 1000 buys
// Phase 2 -- 1000 sells
// Phase 3 -- 250 buy cancels
// Phase 4 -- 250 sell cancels
//
// Buy price (100.00) is below sell price (105.00) so orders do not cross --
// all rest on the book and generate New ERs only, no fills.
//
// clordId() wraps fix.uniqueId() with a realistic multi-component prefix so
// that ClOrdIDs exceed the std::string SSO boundary (~16 bytes), exercising
// the gateway's OpenOrderEntry pool with strings that require pool storage.
// The script is safe to re-run without generating duplicate IDs.

def clordId = { "APM-ALGO-EQUITY-${fix.uniqueId()}" }

session.logon("APM001", "stubpassword", true)

def deadline = System.currentTimeMillis() + 5000
while (!session.isLoggedOn() && System.currentTimeMillis() < deadline) {
    sleep(200)
}
if (!session.isLoggedOn()) {
    out.println "ERROR: logon did not complete within 5 seconds"
    return
}
out.println "Logged on"

def symbol    = "AAPL"
def qty       = 100
def buyPrice  = 100.00
def sellPrice = 105.00

// ----------------------------------------------------------------
// Phase 1: 1000 buy orders
// ----------------------------------------------------------------
def buyIds = (1..1000).collect { clordId() }

buyIds.each { id ->
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID(id))
    nos.set(new quickfix.field.Symbol(symbol))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(qty))
    nos.set(new quickfix.field.Price(buyPrice))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
}
out.println "Phase 1 complete: sent ${buyIds.size()} buy orders"
sleep(500)

// ----------------------------------------------------------------
// Phase 2: 1000 sell orders
// ----------------------------------------------------------------
def sellIds = (1..1000).collect { clordId() }

sellIds.each { id ->
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID(id))
    nos.set(new quickfix.field.Symbol(symbol))
    nos.set(new quickfix.field.Side(quickfix.field.Side.SELL))
    nos.set(new quickfix.field.OrderQty(qty))
    nos.set(new quickfix.field.Price(sellPrice))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
}
out.println "Phase 2 complete: sent ${sellIds.size()} sell orders"
sleep(500)

// ----------------------------------------------------------------
// Phase 3: cancel first 250 buy orders
// ----------------------------------------------------------------
buyIds.take(250).each { origId ->
    def ocr = fix.orderCancelRequest()
    ocr.set(new quickfix.field.ClOrdID(clordId()))
    ocr.set(new quickfix.field.OrigClOrdID(origId))
    ocr.set(new quickfix.field.Symbol(symbol))
    ocr.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    ocr.set(new quickfix.field.OrderQty(qty))
    session.send(ocr)
}
out.println "Phase 3 complete: sent 250 buy cancel requests"
sleep(500)

// ----------------------------------------------------------------
// Phase 4: cancel first 250 sell orders
// ----------------------------------------------------------------
sellIds.take(250).each { origId ->
    def ocr = fix.orderCancelRequest()
    ocr.set(new quickfix.field.ClOrdID(clordId()))
    ocr.set(new quickfix.field.OrigClOrdID(origId))
    ocr.set(new quickfix.field.Symbol(symbol))
    ocr.set(new quickfix.field.Side(quickfix.field.Side.SELL))
    ocr.set(new quickfix.field.OrderQty(qty))
    session.send(ocr)
}
out.println "Phase 4 complete: sent 250 sell cancel requests"
sleep(5000)

session.logout()
out.println "Done."
