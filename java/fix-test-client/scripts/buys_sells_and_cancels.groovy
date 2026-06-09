// Place 1000 buy orders and 1000 sell orders, then cancel the first 250 of each.
//
// Phase 1 — 1000 buys  (ClOrdID: BUY-00001 … BUY-01000)
// Phase 2 — 1000 sells (ClOrdID: SELL-00001 … SELL-01000)
// Phase 3 — 250 buy cancels  (cancels BUY-00001 … BUY-00250)
// Phase 4 — 250 sell cancels (cancels SELL-00001 … SELL-00250)

session.logon()
sleep(2000)

def symbol   = "AAPL"
def qty      = 100
def buyPrice = 100.00
def sellPrice = 105.00

// ----------------------------------------------------------------
// Phase 1: 1000 buy orders
// ----------------------------------------------------------------
def buyIds = (1..1000).collect { String.format("BUY-%05d", it) }

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
def sellIds = (1..1000).collect { String.format("SELL-%05d", it) }

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
buyIds.take(250).eachWithIndex { origId, i ->
    def cancelId = String.format("CXL-BUY-%05d", i + 1)
    def ocr = fix.orderCancelRequest()
    ocr.set(new quickfix.field.ClOrdID(cancelId))
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
sellIds.take(250).eachWithIndex { origId, i ->
    def cancelId = String.format("CXL-SELL-%05d", i + 1)
    def ocr = fix.orderCancelRequest()
    ocr.set(new quickfix.field.ClOrdID(cancelId))
    ocr.set(new quickfix.field.OrigClOrdID(origId))
    ocr.set(new quickfix.field.Symbol(symbol))
    ocr.set(new quickfix.field.Side(quickfix.field.Side.SELL))
    ocr.set(new quickfix.field.OrderQty(qty))
    session.send(ocr)
}
out.println "Phase 4 complete: sent 250 sell cancel requests"
sleep(2000)

session.logout()
out.println "Done."
