from math import ceil, floor
### NRH is based on double-sided
def get_qprac_parameters(NRH):
    nrh_nbo_pairs = [
        (125, 89),
        (250, 217),
        (500, 470),
        (750, 731),
        (1000, 972),
    ]
    for nrh, NBO in nrh_nbo_pairs:
        if NRH == nrh:
            return NBO

### MINT's BAT calculated considering DMQ 
def get_mint_parameters(NRH):
    nrh_bat_pairs = [
        (125, 5),
        (250, 11),
        (500, 24),
        (750, 36),
        (1000, 48),
    ]
    for nrh, BAT in nrh_bat_pairs:
        if NRH == nrh:
            return BAT

def get_prism_parameters(NRH):
    # NRH, M, L, ACT Rate
    nrh_bat_pairs = [
        (250, 9, 79, 48),   # 632 entries
        (500, 7, 41, 72),   # 246 entries
        (750, 7, 11, 72),   # 66 entries
        (1000, 4, 12, 72),  # 36 entries
    ]
    for nrh, M, L, act_rate in nrh_bat_pairs:
        if NRH == nrh:
            return M, L, act_rate

def get_mopac_parameters(NRH):
    # NRH, NBO, SRQ, Prob, Drain
    nrh_bat_pairs = [
        (250, 60, 16, 0.25, 2),
        (500, 152, 16, 0.125, 2),
        (1000, 336, 16, 0.0625, 2),
    ]
    for nrh, NBO, SRQ, Prob, Drain in nrh_bat_pairs:
        if NRH == nrh:
            return NBO, SRQ, Prob, Drain
        
if __name__ == "__main__":
    print(get_prism_parameters(500))


    