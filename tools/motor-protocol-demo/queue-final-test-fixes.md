# Final executable test feedback

Implement bounded fixes after queue-backend-review. Same ownership. Codex WSL compile now succeeds, controller892checks and UART+controllerpass. Queue282checks42failures (mostfixtures andtwo realparserbugs). Do not weaken productioncorrectness to satisfyfixtures.

Production fixes mandatory:
- parseMoveStep max7 but explicitunit+4optional =8tokens. Examplefrontendbuilder `move 1 90 deg 30 60 60 800`. Allowcorrectcount.
- parseMoveStep currentlong->uint16castbeforeplanner wraps66336to800; enforce100..limits.maxCurrentMa BEFOREnarrowing.
- parseVelocityStep max5 butfull `velocity 3 -30 200 60 800` has6; allow6. min4 forvelocity/torque (durationmandatory).
- Timeddefaultpolicy values onlycheckedinsideoptionalargument branch. Movevalidationusesplannerfine; C5defaultmaxRPM30andC6defaultaccel60/current800 MUST be validatedagainstlimits evenifomitted, beforeANYprogramTX. Accepttorquecurrent1..policymax (no100mAfloor). Speeds min0.1 androundednonzero; C5ramp0 is allowedbybackendcurrently whileUImin1: choose documented0..65535, alignbrief forCodexUI. No silentclipping.

Fixture diagnoses fromindependentreview:
- atomicvalidation test expectsrunId1 butalreadyacceptedwait0 thencancel; nextsuccessfulstartisrunId2.
- hex0102 is malformed byte token, errorhex_digit_invalid correct insteadexpectedhex_length.
- rotationtests calltickonlyonce (enterswatchphase), nofreshpostwatchsamplethenexpectmove. Addfeedbackandnextpoll, don'tremoveboundedwatchphase.
- homefailed/movefailed messagecurrentlypollglobalfault emitsfault_active beforeperstep; improveusefulspecificfailure ifdesired orassert failedstate+fault withoutmanufacturingthewronglabel.
- logicalPayload helper stripsALLopcodes whileexpectedTorque/Velocityincludeoneopcode. Preservefirstfunctionbyte, striponlyrepeatedfunctions. Independentexpectedbytesqueue-review-notes.
- hostiletest `can ext 0x12345678 DE` is VALID29-bit (<=1FFFFFFF), expects400wrong; use0x22345678forinvalid. Validacceptedstartleavesqueuerunning soallsubsequentinvalidcasesreturn409not400. Usepureparsefixturesornewrig,don'tcallcancelandintroducestoppending withoutsettling.
- quietdisable fixture needsfreshposition/velocity AFTER F3ACK becausecontrollerstopRequestedMs=ACKtime; same-timepair can'tconfirmstop. Addlaterdistinctpair.

Please report exactremaininguncertainties,don'tclaimtestpass (Codexruns). NoeditCodexowned rawHALtests/frontfiles.
