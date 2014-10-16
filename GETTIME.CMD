/*  gettime.cmd 1995-05-06 mjb  */

    call setlocal

    srvpnm = '\pipe\time'

    call charout , 'Getting time from server at' srvpnm '... '

    call lineout srvpnm, 'time'

    l = linein(srvpnm)
    if l = '' then do
        call lineout , 'failed - no response from server'
        call endlocal
        return 10
        end

    parse value l with rc t
    if rc <> 'OK' then do
        call lineout , 'failed - rc is' rc
        call endlocal
        return 10
        end

    call lineout , t

    call lineout srvpnm

    call endlocal

    return 0
