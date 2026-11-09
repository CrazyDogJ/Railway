// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.h"
#include "UObject/Object.h"
#include "RailwayTrainSaveGame.generated.h"

class ARailwayTrain;

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayTrainSaveGame
{
	GENERATED_BODY()

	static FRailwayTrainSaveGame SaveTrain(const ARailwayTrain* Train);
	
	void LoadTrain(ARailwayTrain* Train) const;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    bool bValid = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    ERailwayDriveMode DriveMode = ERailwayDriveMode::Automatic;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
	FRailwayPlan RailwayPlan;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
	double Distance = 0;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
	float Speed = 0;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
	ERailwayState State = ERailwayState::Idle;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
	double Wait = 0;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
	bool bPreLeaveSent = false;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
	int32 NextStop = 0;
};
